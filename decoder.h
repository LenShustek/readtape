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

#define DEBUG 0                // generate debugging output?
#define TRACEFILE true           // if DEBUG, are we also creating trace file?
#define TRACETRK 0               // for which track
#define MULTITRACK false         // or, are we plotting multi-track analog waveforms?
#define PEAK_STATS true          // accumulate peak timing statistics?

#define DLOG_LINE_LIMIT 5000     // limit for debugging output

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
#include <stddef.h>

#define MAXTRKS 9
#define MAXBLOCK 32768
#define MAXPARMSETS 15
#define MAXPARMS 15
#define MAXPATH 200
#define MAXLINE 400


// Here are lots of of parameters that control the decoding algorithm.
// Some of these are defaults or maxima, for which the currently used values are in the parms_t structure.

#define NRZI_MIDPOINT    0.65        // how far beyond a bit clock is the NRZI "midpoint"
#define NRZI_IBG_SECS    0.0002     // minimum interblock gap (depends on current IPS?)
#define NRZI_RESET_SPEED false      // should we reset the tape speed based on the second peak of a block?

#define PE_IDLE_FACTOR   2.5        // PE: how much of the bit spacing to wait for before considering the track idle
#define IGNORE_POSTAMBLE   5        // PE: how many postable bits to ignore
#define MIN_PREAMBLE       70       // PE: minimum number of peaks (half that number of bits) for a preamble
#define MAX_POSTAMBLE_BITS 40       // PE: maximum number of postamble bits we will remove

#define PKWW_MAX_WIDTH   20         // the peak-detect moving window maximum width, in number of samples
#define PKWW_RISE        0.1        // the required rise in volts that represents a peak (will be adjusted by AGC and peak height)
#define PKWW_PEAKHEIGHT  4.0        // that rise assumes this peak-to-peak voltage

#define PEAK_THRESHOLD   0.005      // volts of difference that define "same peak", scaled by AGC
#define EPSILON_T        1.0e-7     // in seconds, time fuzz factor for comparisons
#define CLKRATE_WINDOW   10         // maximum window width for clock averaging
#define IDLE_TRK_LIMIT   9          // how many tracks must be idle to consider it an end-of-block
#define FAKE_BITS        true       // should we fake bits during a dropout?
#define USE_ALL_PARMSETS false      // should we try to use all the parameter sets, to be able to rate them?
#define AGC_MAX_WINDOW   10         // maximum number of peaks to look back for the min peak
#define AGC_MAX          20 //15         // maximum value of AGC
#define AGC_STARTBASE     5         // starting peak for baseline voltage measurement
#define AGC_ENDBASE      15         // ending peak for baseline voltage measurement

typedef unsigned char byte;
#define NULLP (void *)0

#if DEBUG
#define dlog(...) {debuglog(__VA_ARGS__);}
#else
#define dlog(...) {}
#endif

struct sample_t {       // what we get from the digitizer hardware:
   double time;         //   the time of this sample
   float voltage[MAXTRKS];   //   the voltage level from each track head
};

enum mode_t {
   PE, NRZI, GCR, ALL };

// the track state structure

struct clkavg_t { // structure for keeping track of clock rate
   // we either do window averaging or exponential averaging, depending on parms
   float t_bitspacing[CLKRATE_WINDOW];  // last n bit time spacing
   int bitndx;  // index into t_bitspacing of next spot to use
   float t_bitspaceavg;  // current avg of bit time spacing
};

struct trkstate_t {  // track-by-track decoding state
   int trknum;             // which track number 0..8, where 8=P
   float v_now;            // current voltage

   float v_top;            // top voltage
   double t_top;           // time of top
   float v_lasttop;        // remembered last top voltage

   float v_bot;            // bottom voltage
   double t_bot;           // time of bottom voltage
   float v_lastbot;        // remembered last bottom voltage

   float v_lastpeak;       // last peak (top or bottom) voltage
   double t_lastpeak;      // time of last top or bottom

   float pkww_v[PKWW_MAX_WIDTH];  // the window of sample voltages
   float pkww_minv;        // the minimum voltage in the window
   float pkww_maxv;        // the maximum voltage in in the window
   int pkww_left;          // the index into the left edge sample
   int pkww_right;         // the index into the right edge sample
   int pkww_countdown;     // countdown timer for peak to exit the window

   float v_avg_height;     // average of low-to-high voltage during preamble
   float v_avg_height_sum; // temp for summing the initial average
   int v_avg_height_count; // how many samples went into that average
   float agc_gain;         // the current AGC gain
   float max_agc_gain;     // the highest ever AGC gain
   float v_heights[AGC_MAX_WINDOW];  // last n peak-to-peak voltages
   int heightndx;                // index into v_heights of next spot to use
   double t_lastbit;       // time of last data bit transition
   double t_firstbit;      // time of first data bit transition in the data block
   float t_clkwindow;      // PE: how late, in usec, a clock transition can be before we consider it data
   float t_pulse_adj;      // PE: how much, in usec, to adjust the pulse by based on previous pulse's timing
   struct clkavg_t clkavg; // data for computing average clock
   int datacount;          // how many data bits we've seen
   int peakcount;          // how many peaks (flux reversals) we've seen
   byte lastdatabit;       // the last data bit we recorded
   bool idle;              // are we idle, ie not seeing transitions?
   bool clknext;           // do we expect a PE clock next?
   bool hadbit;            // NRZI: did we have a bit transition since the last check?
   bool datablock;         // are we collecting data?
};

// For PE and GCR, which are self-clocking, the clock for each track is separately computed.
// That allows us to be insensitive to significant head skew.
// For NRZI, which is not self-clocking, we compute one global clock and keep it synchronized
// in frequency and phase to transitions on any track. That allows us to tolerate pretty wide
// changes in tape speed.
struct nrzi_t { // NRZI decode state information
   double t_lastclock;     // time of the last clock
   double t_last_midbit;   // time we did the last mid-bit processing
   struct clkavg_t clkavg; // the current bit rate estimate
   bool datablock;         // are we in a data block?
   bool reset_speed;       // did we reset the speed ourselves?
   int post_counter;       // counter for post-data bit times: CRC is at 4, LRC is at 8
}; // nrzi.

struct parms_t {  // a set of parameters used for decoding a block. We try again with different sets if we get errors.
   int active;             // 1 means this is an active parameter set
   int clk_window;         // how many bit times to average for clock rate; 0 means maybe use exponential averaging
   float clk_alpha;        // weighting for current data in the clock rate exponential weighted average; 0 means use constant
   int agc_window;         // how many peaks to look back for the min peak to set AGC; 0 means maybe use exponential averaging
   float agc_alpha;        // weighting for current data in the AGC exponential weight average; 0 means no AGC
   float min_peak;         // the minimum height of a peak in volts
   float clk_factor;       // PE: how much of a half-bit period to wait for a clock transition
   float pulse_adj;        // how much of the previous pulse's deviation to adjust this pulse by, 0 to 1
   float pkww_bitfrac;     // what fraction of the bit spacing the window width is
   // ...add more dynamic parameters above here, and in the arrays at the top of decoder.c
   char id[4];             // "PRM", to make sure the sructure initialization isn't screwed up
   int tried;              // how many times this parmset was tried
   int chosen;             // how many times this parmset was chosen
}; // array: parmsets_xxx[block.parmset]

extern struct parms_t *parmsetsptr;    // pointer to one of the following sets of decoding parameters
extern struct parms_t parmsets_PE[];
extern struct parms_t parmsets_NRZI[];
extern struct parms_t parmsets_GCR[];
#define PARM parmsetsptr[block.parmset]  // macro for referencing a current parameter
extern char *parmnames[];

enum bstate_t { // the decoding status a block
   BS_NONE,          // no status available yet
   BS_TAPEMARK,      // a tape mark
   BS_BLOCK,         // a good block, but maybe parity errors or faked bits
   BS_MALFORMED };   // a malformed block: different tracks are different lengths

struct blkstate_t {  // state of the block, when we're done
   int tries;           // how many times we tried to decode this block
   int parmset;         // which parameter set we are currently using
   struct results_t {      // the results from the various parameter sets, in order
      enum bstate_t blktype;     // the ending state of the block
      int minbits, maxbits;      // the min/max bits of all the tracks
      float avg_bit_spacing;     // what the average bit spacing was, in secs
      int vparity_errs;          // how many vertical (byte) parity errors it has
      int faked_bits;            // how many faked bits it has
      int errcount;              // how many total errors it has: parity + CRC + LRC
      bool crc_bad, lrc_bad;     // NRZI 800: are crc/lrc ok?
      int crc, lrc;              // NRZI 800; the actual crc anc lrc values in the data
      float alltrk_max_agc_gain; // the maximum AGC gain we used for any track
   } results [MAXPARMSETS]; // results for each parmset we tried
}; // block

void fatal(const char *, ...);
void assert(bool, const char *, ...);
void rlog(const char *, ...);
void breakpoint(void);
char *intcommas(int);
char *longlongcommas(long long int);
byte parity(uint16_t);
void init_trackstate(void);
void init_blockstate(void);
enum bstate_t process_sample(struct sample_t *);
void show_block_errs(int);
void nrzi_output_stats(void);
bool parse_option(char *);
char *modename(void);

extern enum mode_t mode;
extern bool terse, verbose, quiet, multiple_tries;
byte expected_parity;
extern int dlog_lines;
extern double timenow;
extern double interblock_expiration;
extern struct blkstate_t block;
extern uint16_t data[];
extern uint16_t data_faked[];
extern double data_time[];
extern struct nrzi_t nrzi;
extern float bpi, ips;
extern int ntrks;
extern int pkww_width;
extern float sample_deltat;
extern char basefilename[];
extern byte EBCDIC[];

//*
