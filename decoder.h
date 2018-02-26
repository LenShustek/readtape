// file: decoder.h

/**********************************************************************

header file for readtape.c and decoder.c

***********************************************************************
Copyright (C) 2018, Len Shustek

See readtape.c for the merged change log.
***********************************************************************
The MIT License (MIT):
Permission is hereby granted, free of charge,
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

#define DEBUG 0

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

#define NTRKS 9
#define INVERTED false
#define MAXBLOCK 32768
#define MAXPARMSETS  10
#define MAXPATH 150

// Here are lots of of parameters that control the decoding algorithm.
// Some of these are defaults, for which the currently used values are in the parms_t structure.


#define PEAK_THRESHOLD	0.01 			// volts that define "same peak" (TYP: 0.02)
#define BIT_SPACING		12.5e-6		// in seconds, the default bit spacing (1600 BPI x 50 IPS)
#define EPSILON_T			1.0e-7		// in seconds, time fuzz factor for comparisons
#define CLK_FACTOR		1.4       	// how much of a half-bit period to wait for the clock transition.
#define BIT_FACTOR      2.5     		// how much of the bit spacing to wait for before faking bits
#define MAX_AVG_WINDOW  20				// up to how many bit times to include in the clock timing moving average (0 means use defaults)
#define IDLE_TRK_LIMIT	9				// how many tracks must be idle to consider it an end-of-block
#define FAKE_BITS       true    		// should we fake bits during a dropout?
#define MULTIPLE_TRIES	true			// should we do multiple tries to decode a block?
#define USE_ALL_PARMSETS false		// should we try to use all the parameter sets, to be able to rate them?
#define AGC				 	true			// do automatic gain control for weak signals?
#define AGC_MAX			 10			// making it higher causes block 6 to fail!
#define AGC_ALPHA        0.5        // the weighting for the current data in the AGC exponential weighted average


#define IGNORE_POSTAMBLE	5		// how many postable bits to ignore
#define MIN_PREAMBLE		70			// minimum number of peaks (half that number of bits) for a preamble
#define MAX_POSTAMBLE_BITS	40		// maximum number of postable bits we will remove
#define AGC_STARTBASE	10			// starting peak for preamble baseline voltage measurement
#define AGC_ENDBASE     50			// ending peak for preamble baseline voltage measurement


typedef unsigned char byte;
#define NULLP (void *)0

#if DEBUG
#define dlog(...) log(__VA_ARGS__) // debugging log
#else
#define dlog(...)
#endif

struct sample_t {			// what we get from the digitizer hardware:
   double time;			//   the time of this sample
   float voltage[NTRKS];	//   the voltage level from each track head
};

// the track state structure

enum astate_t {
   AS_IDLE, AS_UP, AS_DOWN };

struct trkstate_t {	// track-by-track decoding state
   int trknum;				// which track number 0..8, where 8=P
   enum astate_t astate;	// current state: AS_xxx
   float v_now;  			// current voltage
   float v_top; 			// top voltage
   double t_top; 			// time of top
   float v_lasttop;         // remembered last top voltage
   float v_bot;				// bottom voltage
   double t_bot;			// time of bottom voltage
   float v_lastbot;         // remembered last bottom voltage
   float v_lastpeak;        // last peak (top or bottom) voltage
   double t_lastpeak;		// time of last top or bottom
   float v_avg_height;		// average of low-to-high voltage during preamble
   int v_avg_count;         // how many samples went into that average
   float last_move_threshold;   // the last move threshold we used
   float agc_gain;          // the current AGC gain
   float max_agc_gain;      // the most we ever reduced the move threshold by
   double t_lastbit;		// time of last data bit transition
   double t_firstbit;		// time of first data bit transition in the data block
   double t_clkwindow; 		// how late a clock transition can be, before we consider it data
   float avg_bit_spacing;	// how far apart bits are, on average, in this block (computed at the end)
#if MAX_AVG_WINDOW
   float t_bitspacing[MAX_AVG_WINDOW]; // last n bit time spacing
   int	bitndx;				// index into t_bitspacing of next spot to use
#endif
   float t_bitspaceavg;	// average of last n bit time spacing, or a constant
   int datacount;			// how many data bits we've seen
   int peakcount;			// how many peaks (flux reversals) we've seen
   byte lastdatabit;       // the last data bit we recorded
   byte manchdata;			// the reconstructed manchester encoding
   bool idle;              // are we idle, ie not seeing transitions?
   bool moving;			// are we moving up or down through a transition?
   bool clknext;			// do we expect a clock next?
   bool datablock;			// are we collecting data?
};

struct parms_t {	// a set of parameters used for reading a block. We try again with different ones if we get errors.
   float clk_factor;		// how much of a half-bit period to wait for a clock transition
   int avg_window;			// how many bit times to average for clock rate, 0 means use constant default
   float move_threshold;	// how many volts of deviation means that we have a real signal
   // ...add more dynamic parameters above here
   int tried;          	// how many times this parmset was tried
   int chosen;  		    // how many times this parmset was chosen
}; // array: parmsets[block.parmset]

enum bstate_t { // the decoding status a block
   BS_NONE,        // no status available yet
   BS_TAPEMARK,    // a tape mark
   BS_BLOCK,       // a good block, but maybe parity errors or faked bits
   BS_MALFORMED }; // a malformed block: different tracks are different lengths

struct blkstate_t {	// state of the block, when we're done
   int tries;			 // how many times we tried to decode this block
   int parmset;         // which parm set we are currently using
   struct results_t {      // the results from the various parameter sets
      enum bstate_t blktype;  // the ending state of the block
      int minbits, maxbits;   // the min/max bits of all the tracks
      float avg_bit_spacing;
      int parity_errs;        // how many parity errors it has
      int faked_bits;         // how many faked bits it has
   }
   results [MAXPARMSETS]; // results for each parm set we tried
}; // block

#if 0
void fatal(const char *, ...);
void assert(bool, const char *, ...);
#endif
void fatal(char *, char *);
void assert(bool, char *);
void log(const char *, ...);
byte parity(uint16_t);
void init_trackstate(void);
void init_blockstate(void);
enum bstate_t process_sample(struct sample_t *);
void show_block_errs(int);

//*
