// file: decoder.h
/**********************************************************************

header file for readtape.c and decoder.c

---> See readtape.c for the merged change log.

***********************************************************************
Copyright (C) 2018, Len Shustek
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

#define DEBUG 0                  // debugging output?
#define TRACEFILE false	         // creating trace file?
#define TRACETRK 8		         // for which track
#define DLOG_LINE_LIMIT 1000     // limit on debugging line output

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
#define MAXPARMSETS 15
#define MAXPATH 200
#define VA_WORKS true     // does va_list work reliably in your compiler?

// Here are lots of of parameters that control the decoding algorithm.
// Some of these are defaults, for which the currently used values are in the parms_t structure.

#define NRZI_CLK_DEFAULT (50*800)   // 50 ips x 800 bpi = 40 Khz
#define NRZI_MIDPOINT    0.5        // how far beyond a bit clock is the NRZI "midpoint"
#define NRZI_IBG_SECS    0.0001     // minimum interblock gap
#define PEAK_THRESHOLD	 0.005      // volts that define "same peak" (TYP: 0.02)
#define BIT_SPACING		 12.5e-6		// in seconds, the default bit spacing (1600 BPI x 50 IPS)
#define EPSILON_T		    1.0e-7		// in seconds, time fuzz factor for comparisons
#define CLK_FACTOR		 1.4       	// how much of a half-bit period to wait for the clock transition.
#define IDLE_FACTOR       2.5     	// how much of the bit spacing to wait for before considering the track idle
#define CLKRATE_WINDOW   10         // maximum window for clock averaging
#define IDLE_TRK_LIMIT	 9				// how many tracks must be idle to consider it an end-of-block
#define FAKE_BITS        true    	// should we fake bits during a dropout?
#define USE_ALL_PARMSETS false		// should we try to use all the parameter sets, to be able to rate them?
#define AGC_AVG    	 	 false 		// do automatic gain control for weak signals based on exponential averaging?
#define AGC_MIN          true       // do automatic gain control for weak signals based on min of last n peaks?
#define AGC_MAX			 15			// max agc boost (making it higher causes block 6 to fail!)
#define AGC_ALPHA        0.8        // for AGC_AVG: the weighting for the current data in the AGC exponential weighted average
#define AGC_WINDOW       5          // for AGC_MIN: number of peaks to look back for the min peak

#define IGNORE_POSTAMBLE	5		   // how many postable bits to ignore
#define MIN_PREAMBLE		   70		   // minimum number of peaks (half that number of bits) for a preamble
#define MAX_POSTAMBLE_BITS	40		   // maximum number of postable bits we will remove
#define AGC_STARTBASE	   10		   // starting peak for preamble baseline voltage measurement
#define AGC_ENDBASE        50		   // ending peak for preamble baseline voltage measurement

typedef unsigned char byte;
#define NULLP (void *)0

#if DEBUG
#define dlog(...) if (++dlog_lines < DLOG_LINE_LIMIT) rlog(__VA_ARGS__) // debugging log
#else
#define dlog(...)
#endif

struct sample_t {			// what we get from the digitizer hardware:
   double time;			//   the time of this sample
   float voltage[NTRKS];	//   the voltage level from each track head
};

enum mode_t {
   PE, NRZI, GCR
};

// the track state structure

enum astate_t {
   AS_IDLE, AS_UP, AS_DOWN };
   
struct clkavg_t { // structure for keeping track of clock rate
   // we either do window averaging or exponential averaging, depending on parms
   float t_bitspacing[CLKRATE_WINDOW];  // last n bit time spacing
   int bitndx;  // index into t_bitspacing of next spot to use
   float t_bitspaceavg;  // current avg of bit time spacing
};

struct trkstate_t {	// track-by-track decoding state
   int trknum;				   // which track number 0..8, where 8=P
   enum astate_t astate;	// current state: AS_xxx
   float v_now;  			   // current voltage
   float v_top; 			   // top voltage
   double t_top;           // time of top
   float v_lasttop;        // remembered last top voltage
   float v_bot;				// bottom voltage
   double t_bot;			   // time of bottom voltage
   float v_lastbot;        // remembered last bottom voltage
   float v_lastpeak;       // last peak (top or bottom) voltage
   double t_lastpeak;		// time of last top or bottom
   float v_avg_height;		// average of low-to-high voltage during preamble
   int v_avg_count;        // how many samples went into that average
   float last_move_threshold;   // the last move threshold we used
   float agc_gain;         // the current AGC gain
   float max_agc_gain;     // the most we ever reduced the move threshold by
   float v_heights[AGC_WINDOW];  // last n peak-to-peak voltages
   int heightndx;                // index into v_heights of next spot to use
   double t_lastbit;		   // time of last data bit transition
   double t_firstbit;		// time of first data bit transition in the data block
   float t_clkwindow; 		// how late a clock transition can be, before we consider it data
   float t_pulse_adj;      // how much to adjust the pulse by, based on previous pulse's timing
   struct clkavg_t clkavg; // data for computing average clock
   int datacount;			   // how many data bits we've seen
   int peakcount;	         // how many peaks (flux reversals) we've seen
   byte lastdatabit;       // the last data bit we recorded
   byte manchdata;			// the reconstructed manchester encoding
   bool idle;              // are we idle, ie not seeing transitions?
   bool moving;			   // are we moving up or down through a transition?
   bool clknext;           // do we expect a PE clock next?
   bool datablock;			// are we collecting data?
};

// For PE and GCR, which are self-clocking, the clock for each track is separately computed. 
// That allows us to be insensitive to significant head skew.
// For NRZI, which is not self-clocking, we compute one global clock and keep it synchronized
// in frequency and phase to transitions on any track. That allows us to tolerate pretty wide
// changes in tape speed. It means we can't tolerate much head skew, but that isn't generally 
// an issue for 800 BPI and lower densities with fat bits. 
struct nrzi_t { // NRZI decode state information
   double t_lastclock;     // time of the last clock 
   struct clkavg_t clkavg; // the current bit rate estimate 
   bool datablock;         // are we in a data block?
   int post_counter;       // counter for post-data bit times: CRC is at 4, LRC is at 8
};


struct parms_t {	// a set of parameters used for reading a block. We try again with different ones if we get errors.
   float clk_factor;		   // how much of a half-bit period to wait for a clock transition
   int avg_window;         // how many bit times to average for clock rate; 0 means use exponential averaging
   float clk_alpha;			// weighting for current data in the clock rate exponential weighted average, 0 means use constant
   float pulse_adj_amt;    // how much of the previous pulse's deviation to adjust this pulse by, 0 to 1
   float move_threshold;	// how many volts of deviation means that we have a real signal
   float zero_band;        // how many volts close to zero should be considered zero
   // ...add more dynamic parameters above here
   char id[4];             // "PRM", to make sure the sructure initialization isn't screwed up
   int tried;          	   // how many times this parmset was tried
   int chosen;  		      // how many times this parmset was chosen
}; // array: parmsets[block.parmset]

enum bstate_t { // the decoding status a block
   BS_NONE,          // no status available yet
   BS_TAPEMARK,      // a tape mark
   BS_BLOCK,         // a good block, but maybe parity errors or faked bits
   BS_MALFORMED };   // a malformed block: different tracks are different lengths

struct blkstate_t {	// state of the block, when we're done
   int tries;			   // how many times we tried to decode this block
   int parmset;         // which parm set we are currently using
   struct results_t {      // the results from the various parameter sets
      enum bstate_t blktype;     // the ending state of the block
      int minbits, maxbits;      // the min/max bits of all the tracks
      float avg_bit_spacing;     // what the average bit spacing was, in secs
      int parity_errs;           // how many parity errors it has
      int faked_bits;            // how many faked bits it has
      bool crc_ok, lrc_ok;       // NRZI: are crc/lrc ok?
      int crc, lrc;              // NRZI; the crc anc lrc
      float alltrk_max_agc_gain; // the maximum AGC gain we needed
   }
   results [MAXPARMSETS]; // results for each parm set we tried
}; // block

#if VA_WORKS
void fatal(const char *, ...);
void assert(bool, const char *, ...);
void rlog(const char *, ...);
#else
void fatal(char *, char *);
void assert(bool, char *);
void rlog(const char *, ...);
#endif
void breakapoint(void);
char *intcommas(int);
char *longlongcommas(long long int);

byte parity(uint16_t);
void init_trackstate(void);
void init_blockstate(void);
enum bstate_t process_sample(struct sample_t *);
void show_block_errs(int);

//*
