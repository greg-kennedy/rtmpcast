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
