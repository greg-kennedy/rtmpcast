CFLAGS += -Wall -Wextra
IFLAGS += -I/usr/local/include
LFLAGS += -L/usr/local/lib

all:	rtmpcast testpattern waveform

rtmpcast:	rtmpcast.c
	cc $(IFLAGS) $(CFLAGS) $(LFLAGS) -o rtmpcast rtmpcast.c -lrtmp

testpattern:	testpattern.c
	cc $(IFLAGS) $(CFLAGS) $(LFLAGS) -o testpattern testpattern.c -lrtmp -lx264 -lm

waveform:	waveform.c
	cc $(IFLAGS) $(CFLAGS) $(LFLAGS) -o waveform waveform.c -lrtmp -lx264 -lm -lfdk-aac

clean:
	rm -f rtmpcast testpattern waveform *.o
