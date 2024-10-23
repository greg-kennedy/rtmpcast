/* ***************************************************
rtmpcast: librtmp example code
Greg Kennedy 2021

Sends an input FLV file to a designated RTMP URL.
*************************************************** */
#include <librtmp/rtmp.h>
#include <librtmp/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

// maximum size of a tag is 11 byte header, 0xFFFFFF payload, 4 byte size
#define MAX_TAG_SIZE 11 + 16777215 + 4

#define DEBUG 0

// helper functions
//  parse 24 bits to an unsigned long
static unsigned long u24be(const unsigned char * const p)
{
	return *p << 16 | *(p + 1) << 8 | *(p + 2);
}
//  parse 32 bits to an unsigned long
static unsigned long u32be(const unsigned char * const p)
{
	return *p << 24 | *(p + 1) << 16 | *(p + 2) << 8 | *(p + 3);
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

	// verify two parameters passed
	if (argc != 3) {
		printf("RTMP example code\nUsage:\n\t%s <INPUT.FLV> <URL>\n", argv[0]);
		goto exit;
	}

	/* *************************************************** */
	// allocate a very large buffer for all packets and operations
	unsigned char * tag = malloc(MAX_TAG_SIZE);

	if (tag == NULL) {
		perror("Failed to allocate tag buffer");
		ret = EXIT_FAILURE;
		goto exit;
	}

	/* *************************************************** */
	// Let's open an FLV now
	FILE * flv = fopen(argv[1], "rb");

	if (flv == NULL) {
		perror("Failed to open flv");
		ret = EXIT_FAILURE;
		goto freeTag;
	}

	// make sure it's supported FLV
	fread(tag, 9, 1, flv);

	if (u32be(tag) != 0x464C5601) {
		fputs("Does not appear to be valid FLV1 file\n", stderr);
		ret = EXIT_FAILURE;
		goto closeFLV;
	}

	if (tag[4] & 0x01)
		puts("FLV contains VIDEO");

	if (tag[4] & 0x04)
		puts("FLV contains AUDIO");

	unsigned long flvStartTag = u32be(tag + 5);
	printf("FLV file start offset is %lu\n", flvStartTag);

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
		goto closeFLV;
	}

	RTMP_SetupURL(r, argv[2]);
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

	// Let's install some signal handlers for a graceful exit
	running = 1;
	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGQUIT, sig_handler);
	signal(SIGHUP, sig_handler);

	/* *************************************************** */
	// Ready to start throwing frames at the streamer
	fseek(flv, flvStartTag + 4, SEEK_SET);
	unsigned long prevTimestamp = 0;

	while (running) {
		// read current block
		if (11 != fread(tag, 1, 11, flv)) {
			// failed to read next tag - probably end-of-file.
			if (feof(flv))
				running = 0;
			else {
				perror("Short read looking for next tag header");
				ret = EXIT_FAILURE;
				goto restoreSig;
			}
		} else {
			// Successfully got header.  Parse it.
			unsigned char payloadType = tag[0];
			unsigned long payloadSize = u24be(tag + 1);
			unsigned long timestamp = u24be(tag + 4) | (tag[7] << 24);

			unsigned long streamId = u24be(tag + 8);

			if (DEBUG)
				printf("Position %lu, Type %hhu, Size %lu, Timestamp %lu, Stream %lu\n", ftell(flv), payloadType, payloadSize, timestamp, streamId);

			// Read the rest of the payload.
			if (payloadSize != fread(tag + 11, 1, payloadSize, flv)) {
				perror("Short read trying to get payload");
				ret = EXIT_FAILURE;
				goto restoreSig;
			}

			// Remember the 4-byte tag length at the end.
			//  (This could be folded into the previous call, but for clarity...)
			if (4 != fread(tag + 11 + payloadSize, 1, 4, flv)) {
				perror("Short read trying to get tag size");
				ret = EXIT_FAILURE;
				goto restoreSig;
			}

			// Double-check that we got our tag size right
			if (u32be(tag + (11 + payloadSize)) != 11 + payloadSize) {
				fprintf(stderr, "Read tag size %lu does not match calculated tag size %lu\n", u32be(tag + 11 + payloadSize), 11 + payloadSize);
				ret = EXIT_FAILURE;
				goto restoreSig;
			}

			// Toss into RTMP
			//  cast to char* avoids a warning
			if (RTMP_Write(r, (const char *)tag, 11 + payloadSize + 4) <= 0) {
				fputs("Failed to RTMP_Write\n", stderr);
				ret = EXIT_FAILURE;
				goto restoreSig;
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

			// delay
			//  this is to avoid sending too many frames to the RTMP server,
			//  overwhelming it.  this isn't the most accurate way to do it
			//  but with server buffering it works OK.
			if (prevTimestamp < timestamp) {
				if (DEBUG)
					printf("Sleeping %lu milliseconds\n", timestamp - prevTimestamp);

				usleep(timestamp - prevTimestamp);
				prevTimestamp = timestamp;
			}
		}
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
closeFLV:
	fclose(flv);
freeTag:
	free(tag);
exit:
	return ret;
}
