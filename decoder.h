// file: decoder.h

/**********************************************************************

header file for readtape.c and decoder.c

***********************************************************************
Copyright (C) 2018, Len Shustek

See readtape.c for the merged change log.
***********************************************************************
The MIT License (MIT): Permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*************************************************************************/

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <direct.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>

typedef unsigned char byte;
#define NULLP (void *)0

#define DEBUG 0

#if DEBUG
#define dlog(...) log(__VA_ARGS__) // debugging log
#else
#define dlog(...)
#endif

#define NTRKS 9
#define MAXBLOCK 32768
#define MAXPATH 150

struct sample_t {			// what get from the digitizer hardware:
    double time;			//   the time of this sample
    float voltage[NTRKS];	//   the voltage level from each track head
};

void fatal(char *, char *);
void assert(bool, char *, char *);
void log(const char *, ...);
void init_trackstate(void);
void process_sample(struct sample_t *);
void got_tapemark(void);
void got_crap (int, int);
void got_datablock (uint16_t *, uint16_t *, double *, int, float);

// lots of of parameters that control the decoding algorithm

#define MOVE_THRESHOLD	0.10	// volts of deviation that means something's happening
#define PEAK_THRESHOLD	0.01 	// volts that define "same peak"
#define BIT_SPACING		12.5e-6	// in seconds, the default bit spacing (1600 BPI x 50 IPS)
#define CLK_WINDOW	 	8.5e-6	// in seconds, the default max wait for a clock edge
#define CLK_FACTOR		1.4		// how much of a period to wait for the clock transition.
#define BIT_FACTOR      2.0     // how much of the bit spacing to wait for before faking bits
#define AVG_WINDOW		2		// how many bit times to include in the clock timing moving average (0 means use defaults)
#define IDLE_TRK_LIMIT	9		// how many tracks must be idle to consider it an end-of-block
#define FAKE_BITS       true    // should we fake bits during a dropout?

#define IGNORE_PREAMBLE		5		// how many preamble bits to ignore
#define IGNORE_POSTAMBLE	5		// how many postable bits to ignore
#define MIN_PREAMBLE		70		// minimum number of peaks (half that number of bits) for a preamble

// the track state structure

enum astate_t {
    AS_IDLE, AS_UP, AS_DOWN};

struct trkstate_t {	// track-by-track decoding state
    int trknum;				// which track number 0..8, where 8=P
    enum astate_t astate;	// current state: AS_xxx
    float v_now;  			// current voltage
    float v_top; 			// last top voltage
    double t_top; 			// time of last top
    float v_bot;			// last bottom voltage
    double t_bot;			// time of last bottom
    float v_lastpeak;       // last peak (top or bottom) voltage
    double t_lastpeak;		// time of last top or bottom
    double t_lastbit;		// time of last data bit transition
    double t_firstbit;		// time of first data bit transition in the data block
    double t_clkwindow; 	// how late a clock transition can be, before we consider it data
    float avg_bit_spacing;	// how far apart bits are, on average, in this block (computed at the end)
#if AVG_WINDOW
    float t_bitspacing[AVG_WINDOW]; // last n bit time spacing
    float t_bitspaceavg;	// average of last n bit time spacing
    int	bitndx;				// index into t_bitspacing of next spot to use
#endif
    int datacount;			// how many data bits we've seen
    int peakcount;			// how many peaks (flux reversals) we've seen
    byte lastdatabit;       // the last data bit we recorded
    byte manchdata;			// the reconstructed manchester encoding
    bool idle;              // are we idle, ie not seeing transitions?
    bool moving;			// are we moving up or down through a transition?
    bool clknext;			// do we expect a clock next?
    bool datablock;			// are we collecting data?
};
