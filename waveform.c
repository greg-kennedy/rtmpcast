/* ***************************************************
waveform: libfdk-aac + librtmp example
Greg Kennedy 2021

Generates some soundwaves, encode them with libfdk-aac,
 casts it to RTMP url

Twitch does not allow an audio-only RTMP stream,
 so there is a solid color screen as well

Proper muxing would allow video and audio to run independently
 and interleave using the FLV timestamp field.  However,
 for this simple example, video framerate is tied to
 samplerate / sample_count and emitted together.
 This is approx. 43.066 fps w/ 44100hz and 1024 samples.
*************************************************** */

// push packets to stream
#include <librtmp/rtmp.h>
#include <librtmp/log.h>

// libfdk's AAC encoder header
#include <fdk-aac/aacenc_lib.h>

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
#define WIDTH 640
#define HEIGHT 360

// Everything here is driven by the sample rate and sizes
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define SAMPLE_COUNT 1024

// some calculations in advance
#define TIMESTAMP_INCREMENT (1000.0 / (SAMPLE_RATE / SAMPLE_COUNT))

// turn this on to write a sidecar "out.flv", useful for debugging
#define DEBUG 1

/* ************************************************************************ */
// helper functions
// get "now" in milliseconds
static uint32_t getTimestamp() {
	struct timeval timecheck;
	gettimeofday(&timecheck, NULL);
	return timecheck.tv_sec * 1000 + timecheck.tv_usec / 1000;
}

// write big-endian values to memory area
static uint8_t * u16be(uint8_t * const p, const uint16_t value) {
	*p = value >> 8 & 0xFF;
	*(p + 1) = value & 0xFF;
	return p + 2;
}
static uint8_t * u24be(uint8_t * const p, const uint32_t value) {
	*p = value >> 16 & 0xFF;
	*(p + 1) = value >> 8 & 0xFF;
	*(p + 2) = value & 0xFF;
	return p + 3;
}
static uint8_t * u32be(uint8_t * const p, const uint32_t value) {
	*p = value >> 24 & 0xFF;
	*(p + 1) = value >> 16 & 0xFF;
	*(p + 2) = value >> 8 & 0xFF;
	*(p + 3) = value & 0xFF;
	return p + 4;
}

// deconstruct a host-native double, repack it as big-endian IEEE 754 double
static uint8_t * f64be(uint8_t * const p, const double input) {
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
static uint8_t * pstring(uint8_t * p, const char * const str) {
	uint16_t string_length = strlen(str);
	p = u16be(p, string_length);
	memcpy(p, str, string_length);
	return p + string_length;
}

// AMF (Action Message Format) serializers
static uint8_t * amf_number(uint8_t * const p, const double value) {
	*p = 0x00;
	return f64be(p + 1, value);
}
static uint8_t * amf_boolean(uint8_t * const p, const uint8_t value) {
	*p = 0x01;
	*(p+1) = (value ? 1 : 0);
	return p + 2;
}
static uint8_t * amf_string(uint8_t * const p, const char * const str) {
	*p = 0x02;
	return pstring(p + 1, str);
}
static uint8_t * amf_ecma_array(uint8_t * const p, const uint32_t entries) {
	*p = 0x08;
	return u32be(p + 1, entries);
}
static uint8_t * amf_ecma_array_end(uint8_t * const p) {
	return u24be(p, 0x000009);
}
static uint8_t * amf_ecma_array_entry(uint8_t * const p, const char * const str, const double value) {
	return amf_number(pstring(p, str), value);
}

/* ************************************************************************ */
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

/* ************************************************************************ */
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
static uint8_t * flv_AVCVideoPacket(uint8_t * const p, const unsigned int keyframe, const uint8_t type, const long composition_time)
{
	if (keyframe)
		*p = 0x17;
	else
		*p = 0x27;

	*(p + 1) = type;
	return u24be(p + 2, composition_time);
}

/* ************************************************************************ */
// make a test waveform into the input buffer
//  the pattern is based on value of timestamp, so there's some fun noises
static void build_waveform(INT_PCM * buffer, const uint32_t timestamp)
{
	if (CHANNELS == 1) {
		INT_PCM start_value = buffer[SAMPLE_COUNT - 1];
		for (unsigned long i = 0; i < SAMPLE_COUNT; i ++)
		{
			buffer[i] = start_value + (i+1) * (timestamp % 1024);
		}
	} else {
		INT_PCM start_value = buffer[2 * (SAMPLE_COUNT - 1)];
		for (unsigned long i = 0; i < SAMPLE_COUNT; i++)
		{
			buffer[2 * i] = start_value + (i+1) * (timestamp % 1024);
			buffer[2 * i + 1] = rand();
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
	const uint8_t flvHeader[] = { 0x46, 0x4C, 0x56, 0x01, 0x05, 0, 0, 0, 9, 0, 0, 0, 0 };
	fDebug = fopen("out.flv", "wb");
	fwrite(flvHeader, 1, 13, fDebug);
}
	/* *************************************************** */
	// Initialize the x264 encoder
	//  First set up the parameters struct
	x264_param_t param;
	x264_param_default_preset(&param, "veryfast", "zerolatency");
	param.i_log_level = X264_LOG_INFO;
	param.i_threads = 1;
	param.i_width = WIDTH;
	param.i_height = HEIGHT;
	param.i_fps_num = SAMPLE_RATE;
	param.i_fps_den = SAMPLE_COUNT;
	param.i_keyint_max = (SAMPLE_RATE / SAMPLE_COUNT) * 4; // Twitch likes keyframes every 4 sec or less

	// Enable intra refresh instead of IDR
	//param.b_intra_refresh = 1;

	//Rate control - use CBR not CRF.  allow (up to) 256kbps video rate.
	param.rc.i_rc_method = X264_RC_ABR;
	param.rc.i_bitrate = 256;
	param.rc.i_vbv_max_bitrate = 256;

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
	/* *************************************************** */
	/* *************************************************** */
	// Initialize the AAC encoder
	//  libfdk-aac takes a bunch of these params to set it all up
	//  ordinarily you should check the return value of these but... this is example code
	HANDLE_AACENCODER m_aacenc;
	AACENC_InfoStruct info;
	AACENC_ERROR err;
	// get encoder with support for only basic (AAC-LC) and 1 channel
	err = aacEncOpen(&m_aacenc, 0x01, CHANNELS); if (err != AACENC_OK) { fprintf(stderr, "Failed to open encoder: %d\n", err); goto freePic; }

#define aacSetParam(x, y) { AACENC_ERROR err = aacEncoder_SetParam(m_aacenc, x, y); if (err != AACENC_OK) { fprintf(stderr, "Failed to set param x to y: %d\n", err); goto freePic; } }

	// give me just AAC-LC output (no HC, SSR, SBR etc)
	aacSetParam(AACENC_AOT, AOT_AAC_LC); // AAC-LC
	// Controls the output format of blocks coming from the encoder
	//  this is just raw outputs (no special framing / container)
	aacSetParam(AACENC_TRANSMUX, TT_MP4_RAW);
	// Better quality at the expense of processing power
	//aacSetParam(AACENC_AFTERBURNER,1);

	aacSetParam(AACENC_BITRATE,128 * 1024);
	aacSetParam(AACENC_SAMPLERATE, SAMPLE_RATE);
	// channel arrangement
	aacSetParam(AACENC_CHANNELMODE, (CHANNELS == 2 ? MODE_2 : MODE_1) );
	aacSetParam(AACENC_CHANNELORDER, 1);

	// This strange call is needed to "lock in" the settings for encoding
	err = aacEncEncode(m_aacenc, NULL, NULL, NULL, NULL); if (err != AACENC_OK) { fprintf(stderr, "Failed to initialize encoder: %d\n", err); goto freePic; }

	// Now we have encoder info in a struct and can use it for writing audio packets
	err = aacEncInfo(m_aacenc, &info); if (err != AACENC_OK) { fprintf(stderr, "Failed to copy Encoder Info: %d\n", err); goto freePic; } 
	printf("Opened encoder with these values: maxOutBufBytes = %u, maxAncBytes = %u, inBufFillLevel = %u, inputChannels = %u, frameLength = %u, nDelay = %u, nDelayCore = %u\n", info.maxOutBufBytes, info.maxAncBytes, info.inBufFillLevel, info.inputChannels, info.frameLength, info.nDelay, info.nDelayCore);

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
	p = amf_ecma_array(p, 8);
	p = amf_ecma_array_entry(p, "width", WIDTH);
	p = amf_ecma_array_entry(p, "height", HEIGHT);
	p = amf_ecma_array_entry(p, "framerate", (double)SAMPLE_RATE / SAMPLE_COUNT);
	p = amf_ecma_array_entry(p, "videocodecid", 7);
	p = amf_ecma_array_entry(p, "audiocodecid", 10);
	p = amf_ecma_array_entry(p, "audiodatarate", 128);
	p = amf_ecma_array_entry(p, "audiosamplerate", SAMPLE_RATE);
	//p = amf_ecma_array_entry(p, "audiosamplesize", 16);
	p = amf_boolean(pstring(p, "stereo"), CHANNELS == 2);
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
	x264_nal_t * pp_nal; int pi_nal;
	int header_size = x264_encoder_headers(encoder, &pp_nal, &pi_nal);

	// ready to write the tag
	// First event is the onMetaData, which uses AMF (Action Meta Format)
	//  to serialize basic stream params
	p = flv_TagHeader(tag, 9, 0);

	// Set up an AVC Video Packet (is keyframe, type 0)
	p = flv_AVCVideoPacket(p, 1, 0, 0);
	// write the decoder config record, the initial SPS and PPS
	p = h264_AVCDecoderConfigurationRecord(p,
			pp_nal[0].p_payload + 4,
			pp_nal[0].i_payload,
			pp_nal[1].p_payload + 4,
			pp_nal[1].i_payload);

	// calculate tag size and write it
	tagSize = flv_TagFinish(tag, p);

if (DEBUG) fwrite(tag, 1, tagSize, fDebug);

	if (RTMP_Write(r, (const char *)tag, tagSize) <= 0) {
		fputs("Failed to RTMP_Write\n", stderr);
		ret = EXIT_FAILURE;
		goto freeRTMP;
	}

	// Produce a test image - do this just once here,
	//  it's not the real focus of this test anyway
	memset(pic_in.img.plane[0], 128, WIDTH * HEIGHT);
	memset(pic_in.img.plane[1], 64, WIDTH * HEIGHT / 4);
	memset(pic_in.img.plane[2], 196, WIDTH * HEIGHT / 4);

	/* ************************************************************************** */
	// NOW!!! we have set up the video encoder.
	//  so let's do audio next - the Initial Audio Packet.
	p = flv_TagHeader(tag, 8, 0);
	// 0xA0 for "AAC"
	// 0x0F for flags (44khz, stereo, 16bit)
	*p = 0xAF; p++;
	*p = 0; p++;

	memcpy(p, info.confBuf, info.confSize);
	p += info.confSize;
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
		printf("FRAME %08lu, TIME %011lu\n", frame, (unsigned long)(frame * TIMESTAMP_INCREMENT));

		/* Encode an x264 frame */
		x264_nal_t * nals;
		int i_nals;
		int frame_size = x264_encoder_encode(encoder, &nals, &i_nals, &pic_in, &pic_out);

		if (frame_size <= 0) {
			// error in encoding
			fputs("Error when encoding frame\n", stderr);
			ret = EXIT_FAILURE;
			goto restoreSig;
		}

		// Post our video frame
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

		/* *************************************************** */
		// produce a test waveform
		AACENC_BufDesc in_buf = { 0 }, out_buf = { 0 };
		AACENC_InArgs in_args = { 0 };

		INT_PCM pcmBuffer[SAMPLE_COUNT * CHANNELS] = { 0 };
		in_args.numInSamples     = SAMPLE_COUNT * CHANNELS;

		INT_PCM * in_buffers[]        = { pcmBuffer };
		int in_buffer_sizes[]         = { sizeof(pcmBuffer) };
		int in_buffer_element_sizes[] = { sizeof(INT_PCM) };
		int in_buffer_identifiers[]   = { IN_AUDIO_DATA };

		in_buf.numBufs           = 1;
		in_buf.bufs              = in_buffers;
		in_buf.bufferIdentifiers = in_buffer_identifiers;
		in_buf.bufSizes          = in_buffer_sizes;
		in_buf.bufElSizes        = in_buffer_element_sizes;

		build_waveform(pcmBuffer, frame);

		/* The maximum packet size is 6144 bits aka 768 bytes per channel. */
		uint8_t outBuffer[768 * CHANNELS];

		uint8_t * out_buffers[]        = { outBuffer };
		int out_buffer_sizes[]         = { sizeof(outBuffer) };
		int out_buffer_element_sizes[] = { sizeof(uint8_t) };
		int out_buffer_identifiers[]   = { OUT_BITSTREAM_DATA };

		out_buf.numBufs             = 1;
		out_buf.bufs                = out_buffers;
		out_buf.bufferIdentifiers   = out_buffer_identifiers;
		out_buf.bufSizes            = out_buffer_sizes;
		out_buf.bufElSizes          = out_buffer_element_sizes;

		// ok write some info
		AACENC_OutArgs out_args; // does not need init - is set by encode
		if ( (err = aacEncEncode(m_aacenc, &in_buf, &out_buf, &in_args, &out_args)) != AACENC_OK)
		{
			fprintf(stderr, "Encoding failed: %ld\n", err);
			return 1;
		}

		if (out_args.numOutBytes <= 0) {
			fprintf(stderr, "Encoding returned %d bytes\n", out_args.numOutBytes);
		} else {
			// done, build tag
			p = flv_TagHeader(tag, 8, frame * TIMESTAMP_INCREMENT);
			*p = 0xAF; p++;
			*p = 1; p++;
			memcpy(p, outBuffer, out_args.numOutBytes);
			p += out_args.numOutBytes;

			// calculate tag size and write it
			tagSize = flv_TagFinish(tag, p);

			if (DEBUG) fwrite(tag, 1, tagSize, fDebug);

			//  cast to char* avoids a warning
			if (RTMP_Write(r, (const char *)tag, tagSize) <= 0) {
				fputs("Failed to RTMP_Write audio block\n", stderr);
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
