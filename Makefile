CFLAGS += -Wall -Wextra
IFLAGS += -I/usr/local/include
LFLAGS += -L/usr/local/lib

all:	rtmpcast testpattern

rtmpcast:	rtmpcast.c
	cc $(IFLAGS) $(CFLAGS) $(LFLAGS) -o rtmpcast rtmpcast.c -lrtmp

testpattern:	testpattern.c
	cc $(IFLAGS) $(CFLAGS) $(LFLAGS) -o testpattern testpattern.c -lrtmp -lx264 -lm

clean:
	rm -f rtmpcast testpattern *.o
