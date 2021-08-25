# RTMP Sample Code
C example code for working with librtmp to stream things

Inside here are various experiments to work with librtmp, libx264 etc. directly using C.

## rtmpcast
This is a small tool to broadcast an FLV-container video to an RTMP stream, using librtmp.

Based off these docs:
* the librtmp manpage: https://rtmpdump.mplayerhq.hu/librtmp.3.html
* this Stack Overflow answer on working with librtmp: https://stackoverflow.com/a/25232192/5490719
* and this Stack Overflow answer on working with FLV files: https://stackoverflow.com/a/13803143/5490719

An FLV file is a "packet" format containing some audio and video and other metadata.  Packets are called "tags", and have some 11-bytes header, then variable-sized payload, and then a 4-byte tag size.  FLV now supports x264 and aac, which is how you'd normally work with streaming services anyway.  If you already have mp4 files with these codecs you can use ffmpeg to containerize it in FLV instead:

`ffmpeg -i input.mp4 -c:a copy -c:v copy output.flv`

An RTMP stream expects to be fed FLV tags directly.  It's fairly easy to take an FLV file, skip the header, then read tags sequentially and pass them to librtmp for writing.  That's what this example does!

## testpattern
Generate a testpattern (grayscale bars), encode them with libx264, and push to RTMP stream.

Based off these docs:
* the libx264 source, specifically `example.c` and `x264.h`: https://code.videolan.org/videolan/x264/-/tree/master
* the FLV Spec from Adobe: https://www.adobe.com/content/dam/acom/en/devnet/flv/video_file_format_spec_v10.pdf
* and the AMF Spec also from Adobe: https://www.adobe.com/content/dam/acom/en/devnet/pdf/amf0-file-format-specification.pdf

libx264 is a software-based library for encoding an uncompressed stream of images to h.264 formatted compressed video.  An h.264 stream is a series of "NALU"s (Network Abstraction Layer Unit) which can contain different types of data - a compressed frame, some metadata about how to decompress things, etc.  Meanwhile FLV is a "container format": it wraps the raw h.264 stream in its own "packets", each with one or more NALUs inside.

The major difficulty here is working out the exact h.264 parameters needed to work within FLV.  The short list is:

    // Muxing parameters to x264
    param.b_aud = 0; // DO NOT generate Access Unit Delimiters
    param.b_repeat_headers = 1; // Put SPS/PPS before each keyframe automatically
    param.b_annexb = 0; // Annex B uses startcodes before NALU, but we want sizes

From this it is possible to collect outputs from `x264_encoder_encode()`, assign a correct timestamp, and use them as payload for FLV tags.  Note, also, the "AVC Decoder Configuration Record" (FLV video tag, type 0) which should be sent before any h264 frames are used.  It contains a bit of info and then the first SPS and PPS NALs.

## waveform
Generate a signed-16bit PCM waveform, encode it with libfdk-aac, and push it to RTMP.

Based off these docs:
* the libfdk-aac user manual: https://github.com/mstorsjo/fdk-aac/blob/master/documentation/aacEncoder.pdf
* and the rest of the libfdk-aac source, esp. `aac-enc.c` (example) and the include files: https://github.com/mstorsjo/fdk-aac

libfdk-aac is a port of Android's AAC encoder/decoder provided by Fraunhofer IIS.  It is a semi-successor to the MP3 format with better compression properties and various other bells and whistles.  RTMP streams (and FLV files) can contain an AAC stream audio track in either Mono or Stereo.

AAC encodes a block of 1024 samples at a time, so there is an input buffer with 1024 (or 2048, for stereo) samples, and then this is passed to aacEncEncode to build an output buffer.  The contents of output buffer are packed into FLV audio tags and pushed into `RTMP_write()`.

Unfortunately an audio-only RTMP stream is not supported on Twitch or many other platforms.  As a result parts of the previous x264 example are included to build a static video image (solid orange frame) and added to the RTMP stream along with the audio.  No attempt is made at muxing streams of diffrent framerate here.  Instead, the video framerate is tied to the audio, at `SAMPLE_RATE / SAMPLE_COUNT` and both tags sent with the same timestamp.  For the 44100hz this gives a 43.06 FPS output stream.
