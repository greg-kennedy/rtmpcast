/* ***************************************************
testpattern: lxbi264 + librtmp example
Greg Kennedy 2021

Generates a test pattern, encodes it with x264,
 casts it to RTMP url
*************************************************** */

// push packets to stream
#include <librtmp/rtmp.h>
#include <librtmp/log.h>

// other necessary includes
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include <stdint.h>

#include <sys/time.h>

// h.264 encoder lib
//  this requires stdint.h first or else it complains...
#include <x264.h>

// maximum size of a tag is 11 byte header, 0xFFFFFF payload, 4 byte size
#define MAX_TAG_SIZE 11 + 16777215 + 4

// video output parameters
//#define WIDTH 1920
//#define HEIGHT 1080
#define WIDTH 640
#define HEIGHT 360
#define FPS 24

// some calculations in advance
#define TIMESTAMP_INCREMENT (1000.0 / FPS)

// turn this on to write a sidecar "out.flv", useful for debugging
#define DEBUG 1

// get "now" in milliseconds
static uint32_t getTimestamp()
{
	struct timeval timecheck;
	gettimeofday(&timecheck, NULL);
	return timecheck.tv_sec * 1000 + timecheck.tv_usec / 1000;
}

// helper functions
//  write uint16_t into 16 bits big-endian
static uint8_t * u16be(uint8_t * const p, const uint16_t value)
{
	*p = value >> 8 & 0xFF;
	*(p + 1) = value & 0xFF;
	return p + 2;
}

//  write uint32_t into 24 bits big-endian
static uint8_t * u24be(uint8_t * const p, const uint32_t value)
{
	*p = value >> 16 & 0xFF;
	*(p + 1) = value >> 8 & 0xFF;
	*(p + 2) = value & 0xFF;
	return p + 3;
}

//  write uint32_t into 32 bits big-endian
static uint8_t * u32be(uint8_t * const p, const uint32_t value)
{
	*p = value >> 24 & 0xFF;
	*(p + 1) = value >> 16 & 0xFF;
	*(p + 2) = value >> 8 & 0xFF;
	*(p + 3) = value & 0xFF;
	return p + 4;
}

// deconstruct a host-native double, repack it as big-endian IEEE 754 double
//  this attempts to figure out endianness of the input by casting it to uint8_t
static uint8_t * f64be(uint8_t * const p, const double input)
{
	const uint8_t * const value = (uint8_t *)&input;
	static const double testVal = 1;
	static const uint8_t testBE[8] = { 0x3F, 0xF0 };
	static const uint8_t testLE[8] = { 0, 0, 0, 0, 0, 0, 0xF0, 0x3F };

	if (memcmp(&testVal, testBE, 8) == 0) {
		// already in big-endian
		memcpy(p, value, 8);
	} else { // if (memcmp(&testVal, testLE, 8) == 0)
		// byteswap
		for (int i = 0; i < 8; i++)
			p[i] = value[7 - i];
	}

	return p + 8;
}

// "pascal" string (uint16 strlen, string content)
static uint8_t * pstring(uint8_t * const p, const char * const str)
{
	uint16_t string_length = strlen(str);
	*p = string_length >> 8 & 0xFF;
	*(p + 1) = string_length & 0xFF;
	memcpy(p + 2, str, string_length);
	return p + 2 + string_length;
}

// AMF (Action Message Format) serializers
//  Number (double floating-point)
static uint8_t * amf_number(uint8_t * const p, const double value)
{
	*p = 0x00;
	return f64be(p + 1, value);
}

//  String
static uint8_t * amf_string(uint8_t * const p, const char * const str)
{
	*p = 0x02;
	return pstring(p + 1, str);
}

// Beginning of an Associative Array
static uint8_t * amf_ecma_array(uint8_t * const p, const uint32_t entries)
{
	*p = 0x08;
	return u32be(p + 1, entries);
}

// Closing an Associative Array (final entry)
static uint8_t * amf_ecma_array_end(uint8_t * const p)
{
	return u24be(p, 0x000009);
}

// Entry to an array, as string key and double value
static uint8_t * amf_ecma_array_entry(uint8_t * const p, const char * const str, const double value)
{
	return amf_number(pstring(p, str), value);
}

// some h264 output helpers

// AVCDecoder record - some of this data comes out of the SPS for this block
static uint8_t * h264_AVCDecoderConfigurationRecord(
	uint8_t * p,
	const uint8_t * const sps,
	const uint16_t sps_length,
	const uint8_t * const pps,
	const uint16_t pps_length)
{
	*p = 0x01;	// version
	*(p + 1) = sps[1];	// Required profile ID
	*(p + 2) = sps[2];	// Profile compatibility
	*(p + 3) = sps[3];	// AVC Level (3.0)
	*(p + 4) = 0b11111100 | 0b11;	// NAL lengthSizeMinusOne (4 bytes)
	*(p + 5) = 0b11100000 | 1;	// number of SPS sets
	p += 6;

	// write the SPS - length (uint16), then data
	p = u16be(p, sps_length);
	memcpy(p, sps, sps_length);
	p += sps_length;

	// write the PPS now
	p = u16be(p, pps_length);
	memcpy(p, pps, pps_length);
	p += pps_length;

	return p;
}

// sets up the first 11 bytes of a tag
//  returns pointer to the start of payload area
static uint8_t * flv_TagHeader(uint8_t * p, const uint8_t type, const uint32_t timestamp)
{
	*p = type; p ++; // message type
	// tag[1 - 3] are the message size, which we don't know yet
	p += 3;

	// FLV timestamp is written in an odd format
	p = u24be(p, timestamp & 0x00FFFFFF);
	*p = timestamp >> 24 & 0xFF; p ++;

	return u24be(p, 0); // stream ID
}

// Finishes a tag (corrects Payload Size in bytes 1-3, and appends Tag Size)
//  Returns complete tag size, ready for writing
static uint32_t flv_TagFinish(uint8_t * tag, uint8_t * p)
{
	uint32_t payloadSize = p - (tag + 11);
	u24be(tag + 1, payloadSize);
	u32be(p, 11 + payloadSize);

	return 11 + payloadSize + 4;
}

// FLV Video Packet (AVC format)
//  Composition Time is 0 for all-I frames, but otherwise should be the time diff. between PTS and DTS
static uint8_t * flv_AVCVideoPacket(uint8_t * const p, const unsigned int keyframe, const unsigned int type, const long composition_time)
{
	if (keyframe)
		*p = 0x17;
	else
		*p = 0x27;

	*(p + 1) = type;
	return u24be(p + 2, composition_time);
}


// make a test pattern into pic_in
//  the pattern is based on value of timestamp, so there's some motion
static void build_picture(x264_picture_t * pic, const uint32_t timestamp)
{
	//int luma_size = WIDTH * HEIGHT;
	//int chroma_size = WIDTH * HEIGHT / 4;

	// luma
	for (unsigned int y = 0; y < HEIGHT; y ++) {
		for (unsigned int x = 0; x < WIDTH; x ++)
			pic->img.plane[0][y * WIDTH + x] = (y + timestamp) % 256;
	}

	// chroma
	for (unsigned int y = 0; y < HEIGHT / 2; y ++) {
		for (unsigned int x = 0; x < WIDTH / 2; x ++) {
			pic->img.plane[1][y * WIDTH / 2 + x] = 127;
			pic->img.plane[2][y * WIDTH / 2 + x] = 127;
		}
	}
}

// Flag to indicate whether we should keep playing the movie
//  Set to 0 to close the program
static int running;
// replacement signal handler that sets running to 0 for clean shutdown
static void sig_handler(int signum)
{
	fprintf(stderr, "Received signal %d (%s), exiting.\n", signum, strsignal(signum));
	running = 0;
}

/* *************************************************** */
int main(int argc, char * argv[])
{
	int ret = EXIT_SUCCESS;

	// verify one parameter passed
	if (argc != 2) {
		printf("X264 + RTMP example code\nUsage:\n\t%s <URL>\n", argv[0]);
		goto exit;
	}

	FILE * fDebug;

if (DEBUG) {
	const uint8_t flvHeader[] = { 0x46, 0x4C, 0x56, 0x01, 0x01, 0, 0, 0, 9, 0, 0, 0, 0 };
	fDebug = fopen("out.flv", "wb");
	fwrite(flvHeader, 1, 13, fDebug);
}
	/* *************************************************** */
	// Initialize the x264 encoder
	//  First set up the parameters struct
	x264_param_t param;
	x264_param_default_preset(&param, "veryfast", "zerolatency");
	param.i_log_level = X264_LOG_DEBUG;
	param.i_threads = 1;
	param.i_width = WIDTH;
	param.i_height = HEIGHT;
	param.i_fps_num = FPS;
	param.i_fps_den = 1;
	param.i_keyint_max = FPS;

	// Enable intra refresh instead of IDR
	//param.b_intra_refresh = 1;

	//Rate control:
	param.rc.i_rc_method = X264_RC_CRF;
	param.rc.f_rf_constant = 25;
	param.rc.f_rf_constant_max = 35;

	// Control x264 output for muxing
	param.b_aud = 0; // do not generate Access Unit Delimiters
	param.b_repeat_headers = 1; // Do not put SPS/PPS before each keyframe.
	param.b_annexb = 0; // Annex B uses startcodes before NALU, but we want sizes

	// constraints
	x264_param_apply_profile(&param, "baseline");

	/* *************************************************** */
	// All done setting up params!  Let's open an encoder
	x264_t * encoder = x264_encoder_open(&param);
	// can free the param struct now
	x264_param_cleanup(&param);

	// These are the two picture structs.  Input must be alloc()
	//  Output will be created by the encode process
	x264_picture_t pic_in, pic_out;
	x264_picture_alloc(&pic_in, X264_CSP_I420, WIDTH, HEIGHT);

	/* *************************************************** */
	// allocate a very large buffer for all packets and operations
	uint8_t * const tag = malloc(MAX_TAG_SIZE);

	if (tag == NULL) {
		perror("Failed to allocate tag buffer");
		ret = EXIT_FAILURE;
		goto freePic;
	}

	/* *************************************************** */
	// Increase the log level for all RTMP actions
	RTMP_LogSetLevel(RTMP_LOGINFO);
	RTMP_LogSetOutput(stderr);
	/* *************************************************** */
	// Init RTMP code
	RTMP * r = RTMP_Alloc();
	RTMP_Init(r);

	if (r == NULL) {
		fputs("Failed to create RTMP object\n", stderr);
		ret = EXIT_FAILURE;
		goto freeTag;
	}

	RTMP_SetupURL(r, argv[1]);
	RTMP_EnableWrite(r);

	// Make RTMP connection to server
	if (! RTMP_Connect(r, NULL)) {
		fputs("Failed to connect to remote RTMP server\n", stderr);
		ret = EXIT_FAILURE;
		goto freeRTMP;
	}

	// Connect to RTMP stream
	if (! RTMP_ConnectStream(r, 0)) {
		fputs("Failed to connect to RTMP stream\n", stderr);
		ret = EXIT_FAILURE;
		goto freeRTMP;
	}

	// track the fd for rtmp
	int fd = RTMP_Socket(r);
	struct timeval tv = {0, 0};

	// READY to send the first packet!
	// First event is the onMetaData, which uses AMF (Action Meta Format)
	//  to serialize basic stream params
	uint8_t * p = flv_TagHeader(tag, 18, 0);

	// script data type is "onMetaData"
	p = amf_string(p, "onMetaData");
	// associative array with various stream parameters
	p = amf_ecma_array(p, 4);
	p = amf_ecma_array_entry(p, "width", WIDTH);
	p = amf_ecma_array_entry(p, "height", HEIGHT);
	p = amf_ecma_array_entry(p, "framerate", FPS);
	p = amf_ecma_array_entry(p, "videocodecid", 7);
	// finalize the array
	p = amf_ecma_array_end(p);

	// calculate tag size and write it
	uint32_t tagSize = flv_TagFinish(tag, p);

if (DEBUG) fwrite(tag, 1, tagSize, fDebug);

	if (RTMP_Write(r, (const char *)tag, tagSize) <= 0) {
		fputs("Failed to RTMP_Write\n", stderr);
		ret = EXIT_FAILURE;
		goto freeRTMP;
	}

	// write the h.264 header now
	x264_nal_t * pp_nal;
	int pi_nal;
	int header_size = x264_encoder_headers(encoder, &pp_nal, &pi_nal);

	if (header_size <= 0) {
		// technically 0 is not an error BUT we call it one anyway
		fputs("Failed to call x264_encode_headers\n", stderr);
		ret = EXIT_FAILURE;
		goto freeRTMP;
	}

	// locate the SPS and PPS
	int sps_id = -1, pps_id = -1;

	for (int i = 0; i < pi_nal; i ++) {
		if (pp_nal[i].i_type == NAL_SPS) {
			if (sps_id > -1) {
				fputs("ERROR: stream contains multiple SPS, not supported\n", stderr);
				ret = EXIT_FAILURE;
				goto freeRTMP;
			}

			sps_id = i;
		} else if (pp_nal[i].i_type == NAL_PPS) {
			if (pps_id > -1) {
				fputs("ERROR: stream contains multiple PPS, not supported\n", stderr);
				ret = EXIT_FAILURE;
				goto freeRTMP;
			}

			pps_id = i;
		}
	}

	//
	if (sps_id == -1 || pps_id == -1) {
		fputs("ERROR: x264_encoder_headers missing SPS or PPS\n", stderr);
		ret = EXIT_FAILURE;
		goto freeRTMP;
	}

	// ready to write the tag
	// First event is the onMetaData, which uses AMF (Action Meta Format)
	//  to serialize basic stream params
	p = flv_TagHeader(tag, 9, 0);

	// Set up an AVC Video Packet (is keyframe, type 0)
	p = flv_AVCVideoPacket(p, 1, 0, 0);
	// write the decoder config record, the initial SPS and PPS
	p = h264_AVCDecoderConfigurationRecord(p,
			pp_nal[sps_id].p_payload + 4,
			pp_nal[sps_id].i_payload,
			pp_nal[pps_id].p_payload + 4,
			pp_nal[pps_id].i_payload);

	// calculate tag size and write it
	tagSize = flv_TagFinish(tag, p);

if (DEBUG) fwrite(tag, 1, tagSize, fDebug);

	if (RTMP_Write(r, (const char *)tag, tagSize) <= 0) {
		fputs("Failed to RTMP_Write\n", stderr);
		ret = EXIT_FAILURE;
		goto freeRTMP;
	}

	// Let's install some signal handlers for a graceful exit
	running = 1;
	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGQUIT, sig_handler);
	signal(SIGHUP, sig_handler);
	/* *************************************************** */
	// Ready to start throwing frames at the streamer

	// Current frame
	unsigned long frame = 0;

	// Starting timestamp of our video
	uint32_t start = getTimestamp();

	while (running) {
		// Produce a test image
		build_picture(&pic_in, frame);

		/* Encode an x264 frame */
		x264_nal_t * nals;
		int i_nals;
		int frame_size = x264_encoder_encode(encoder, &nals, &i_nals, &pic_in, &pic_out);

		if (frame_size < 0) {
			// error in encoding
			fputs("Error when encoding frame\n", stderr);
			ret = EXIT_FAILURE;
			goto restoreSig;
		} else if (frame_size > 0) {
			// got an encoded frame
			// Toss into RTMP
			//  this means building a tag of the correct type and throwing the NAL into it
			p = flv_TagHeader(tag, 9, frame * TIMESTAMP_INCREMENT);

			// write every NALU to the packet for this pic
			//  x264 guarantees all p_payload are sequential
			p = flv_AVCVideoPacket(p, pic_out.b_keyframe, 1, 0);
			memcpy(p, nals[0].p_payload, frame_size);
			p += frame_size;

			// calculate tag size and write it
			tagSize = flv_TagFinish(tag, p);

if (DEBUG) fwrite(tag, 1, tagSize, fDebug);

			//  cast to char* avoids a warning
			if (RTMP_Write(r, (const char *)tag, tagSize) <= 0) {
				fputs("Failed to RTMP_Write a frame\n", stderr);
				ret = EXIT_FAILURE;
				goto restoreSig;
			}
		}

		// Handle any packets from the remote to us.
		//  We will use select() to see if packet is waiting,
		//  then read it and dispatch to the handler.
		fd_set set;
		FD_ZERO(&set);
		FD_SET(fd, &set);

		if (select(fd + 1, &set, NULL, NULL, &tv) == -1) {
			perror("Error calling select()");
			ret = EXIT_FAILURE;
			goto restoreSig;
		}

		// socket is present in read-ready set, safe to call RTMP_ReadPacket
		if (FD_ISSET(fd, &set)) {
			RTMPPacket packet = { 0 };

			if (RTMP_ReadPacket(r, &packet) && RTMPPacket_IsReady(&packet)) {
				// this function does all the internal stuff we need
				RTMP_ClientPacket(r, &packet);
				RTMPPacket_Free(&packet);
			}
		}

		// frame count go up
		frame ++;

		// the time to sleep is the duration between target framestamp and now
		int64_t delay_time = (frame * TIMESTAMP_INCREMENT) - (getTimestamp() - start);
		if (delay_time > 0) {
			//printf("Sleeping for %ld seconds\n", delay_time);
			usleep(1000 * delay_time);
		}
	}

	/* Flush delayed frames for a clean shutdown */
	/*
	   while( x264_encoder_delayed_frames( h ) )
	   {
	   i_frame_size = x264_encoder_encode( h, &nal, &i_nal, NULL, &pic_out );
	   if( i_frame_size < 0 )
	   goto fail;
	   else if( i_frame_size )
	   {
	   if( !fwrite( nal->p_payload, i_frame_size, 1, stdout ) )
	   goto fail;
	   }
	   }
	 */

	// send the end-of-stream indicator
	p = flv_TagHeader(tag, 9, frame * TIMESTAMP_INCREMENT);
	// write the empty-body "stream end" tag
	p = flv_AVCVideoPacket(p, 1, 2, 0);
	// calculate tag size and write it
	tagSize = flv_TagFinish(tag, p);

if (DEBUG) fwrite(tag, 1, tagSize, fDebug);

	//  cast to char* avoids a warning
	if (RTMP_Write(r, (const char *)tag, tagSize) <= 0) {
		fputs("Failed to RTMP_Write\n", stderr);
		ret = EXIT_FAILURE;
	}

	/* *************************************************** */
	// CLEANUP CODE
	// restore signal handlers
restoreSig:
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	// Shut down
freeRTMP:
	RTMP_Free(r);
freeTag:
	free(tag);
freePic:
	x264_picture_clean(&pic_in);
if (DEBUG) fclose(fDebug);
exit:
	return ret;
}
