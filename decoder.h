// file: decoder.h
/******************************************************************************

   header file for readtape.c, decoder.c, and friends

---> See readtape.c for the merged change log.

*******************************************************************************
Copyright (C) 2018, Len Shustek

The MIT License (MIT): Permission is hereby granted, free of charge, to any
person obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#define DEBUG false                // generate debugging output?

#define TRACEFILE (true & DEBUG)   // if DEBUG, are we also creating trace file?
#define TRACETRK 4                 // which track gets special attention?
#define TRACEALL true              // are we plotting all analog waveforms? Otherwise just one: TRACETRK
#define TRACESCALE 3.0             // scaling factor for voltages on the chart

#define PEAK_STATS true             // accumulate NRZI/GCR peak timing statistics?
#define DESKEW (true & PEAK_STATS)  // also add code for optional track deskewing?

#define DLOG_LINE_LIMIT 2000     // limit for debugging output

#define TRACING (DEBUG && trace_on && t->trknum == TRACETRK)
#define TRACE(var,time,tickdirection,t) {if(TRACEFILE) trace_event(trace_##var, time, tickdirection, t);}
enum trace_names_t { //** MUST MATCH tracevals in decoder.c !!!
   trace_peak, trace_data, trace_avgpos, trace_zerpos, trace_adjpos, trace_midbit, trace_clkedg, trace_datedg, trace_clkwin, trace_clkdet };
#define UPTICK 0.75f
#define DNTICK -0.75f

#define _FILE_OFFSET_BITS 64     // on Linux, make file offsets be 64 bits
#include <stdio.h>
#if defined(_WIN32)
#include <direct.h>              // on Windows, this is needed for mkdir
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <float.h>
typedef unsigned char byte;
#include "csvtbin.h"

typedef unsigned char bool; // we don't use stdbool.h so we can have "unknown" as a value
#define true 1
#define false 0
#define unknown 0xff

#define MAXTRKS 10
#define MAXBLOCK 32768
#define MAXPARMSETS 15
#define MAXPARMS 15
#define MAXPATH 300
#define MAXLINE 400

#define MAXSKEWSAMP 30     // maximum skew amount in number of samples
#define MAXSKEWBLKS 10     // maximum blocks to preprocess to calibrate skew
#define MINSKEWTRANS 100   // the minumum number of transitions we would like to base skew calibration on

// Here are lots of of parameters that control the decoding algorithm.
// Some of these are defaults or maxima, for which the currently used values are in the parms_t structure.
// More values can be moved into the parms_t structure if they need to change from tape to tape.

#define NRZI_MIDPOINT    0.65 //.65       // how far beyond a bit clock is our NRZI "midpoint" for looking for transitions
#define NRZI_IBG_SECS    200e-6     // minimum interblock gap in seconds (should depend on current IPS?)
#define NRZI_RESET_SPEED false      // should we reset the tape speed based on the second peak of a block?

#define GCR_IDLE_THRESH  4.00       // more than these bit times without a peak mean the track is idle
//#define GCR_2ZERO_THRESH 2.3 //2.50 //2.42       // more than these bit times without a peak means 2 zero bits intervened
//#define GCR_1ZERO_THRESH 1.4 //1.50 //1.475      // more than these bit times without a peak means 1 zero bit intervened
#define GCR_IBG_SECS     200e-6     // minimum interblock gap in seconds (should depend on current IPS?)

#define PE_IDLE_FACTOR   2.5        // PE: how much of the bit spacing to wait for before considering the track idle
#define IGNORE_POSTAMBLE   5        // PE: how many postable bits to ignore
#define MIN_PREAMBLE       70       // PE: minimum number of peaks (half that number of bits) for a preamble
#define MAX_POSTAMBLE_BITS 40       // PE: maximum number of postamble bits we will remove

#define PKWW_MAX_WIDTH   20         // the peak-detect moving window maximum width, in number of samples
#define PKWW_PEAKHEIGHT  4.0f       // the assumed peak-to-peak (2x top or bot height) voltage for the pkww_rise parameter

#define PEAK_THRESHOLD   0.005f     // volts of difference that define "same peak", scaled by AGC
#define CLKRATE_WINDOW   50         // maximum window width for clock averaging
#define FAKE_BITS        true       // should we fake bits during a dropout?
#define USE_ALL_PARMSETS false      // should we try to use all the parameter sets, to be able to rate them?
#define AGC_MAX_WINDOW   10         // maximum number of peaks to look back for the min peak
#define AGC_MAX          20 //15         // maximum value of AGC
#define AGC_STARTBASE     5         // starting peak for baseline voltage measurement
#define AGC_ENDBASE      15         // ending peak for baseline voltage measurement

#define NULLP (void *)0
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#if DEBUG
#define dlog(...) {debuglog(__VA_ARGS__);}
#else
#define dlog(...) {}
#endif
#define TICK(x) ((x - torigin) / sample_deltat - 1)

struct sample_t {       // what we get from the digitizer hardware:
   double time;            // the time of this sample
   float voltage[MAXTRKS]; // the voltage level from each track head
};

struct clkavg_t { // structure for keeping track of clock rate
   // For PE and GCR, there is one of these for each track.
   // For NRZI there is is only one of these, in the nrzi_t structure.
   // We either do window averaging or exponential averaging, depending on parms.
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
   float t_peakdelta;      // delta time between most recent peaks
   float t_peakdeltaprev;  // the previous delta time between peaks

   float pkww_v[PKWW_MAX_WIDTH];  // the window of sample voltages
   float pkww_minv;        // the minimum voltage in the window
   float pkww_maxv;        // the maximum voltage in in the window
   int pkww_left;          // the index into the left edge sample
   int pkww_right;         // the index into the right edge sample
   int pkww_countdown;     // countdown timer for peak to exit the window

   float v_avg_height;     // average of low-to-high voltage during preamble
   float v_avg_height_sum; // temp for summing the initial average
   int v_avg_height_count; // how many samples went into that average
   float agc_gain;         // the current AGC gain, based on recent peaks
   float max_agc_gain;     // the highest ever AGC gain
   float v_heights[AGC_MAX_WINDOW];  // last n peak-to-peak voltages
   int heightndx;                // index into v_heights of next spot to use

   double t_lastbit;       // time of last data bit transition
   double t_firstbit;      // time of first data bit transition in the data block
   double t_lastclock;     // GCR: the time of the last clock
   double t_last_midbit;   // GCR: the time we did the last mid-bit processing
   int consecutive_zeroes; // GCR: how many consecutive all-track zeroes we have seen
   float t_clkwindow;      // PE: how late, in usec, a clock transition can be before we consider it data
   float t_pulse_adj;      // PE: how much, in usec, to adjust the pulse by based on previous pulse's timing
   struct clkavg_t clkavg; // PE and GCR: data for computing average clock
   int datacount;          // how many data bits we've seen
   int peakcount;          // how many peaks (flux reversals) we've seen
   byte lastdatabit;       // the last data bit we recorded
   bool idle;              // are we idle, ie not seeing transitions?
   bool clknext;           // PE: do we expect a clock next?
   bool hadbit;            // NRZI: did we have a bit transition since the last check?
   bool datablock;         // PE and GCR are we collecting data on this track?

   byte lastbits;          // GCR: accumulate the last bits we decoded, so we can recognize control subgroups on the fly
   int  resync_bitcount;   // GCR: how many bits into resync are we?
};

// For PE and GCR, which are self-clocking, the clock for each track is separately computed.
// That allows us to be insensitive to significant head skew.

// For NRZI, which is not self-clocking, we compute one global clock and keep it synchronized
// in frequency and phase to transitions on any track. That allows us to tolerate pretty wide
// changes in tape speed. We compensate for clock skew by (if the -deskew option is specified)
// by precomputing how much to delay the data from each head.
// For PE, each track in independent and has its own clkavg_t structure.

struct nrzi_t { // NRZI (and GCR) decode state information
   double t_lastclock;     // time of the last clock
   double t_last_midbit;   // time we did the last mid-bit processing
   struct clkavg_t clkavg; // the current bit rate estimate
   bool datablock;         // are we in a data block?
   bool reset_speed;       // did we reset the speed ourselves?
   int post_counter;       // not GCR: counter for post-data bit times: CRC is at 4, LRC is at 8
}; // nrzi.

struct parms_t {  // a set of parameters used for decoding a block. We try again with different sets if we get errors.
   int active;             // 1 means this is an active parameter set
   int clk_window;         // how many bit times to average for clock rate; 0 means maybe use exponential averaging
   float clk_alpha;        // weighting for current data in the clock rate exponential weighted average; 0 means use constant
   int agc_window;         // how many peaks to look back for the min peak to set AGC; 0 means maybe use exponential averaging
   float agc_alpha;        // weighting for current data in the AGC exponential weight average; 0 means no AGC
   float min_peak;         // the minimum height of a peak in volts, above or below 0 volts (not relative!)
   float clk_factor;       // PE: how much of a half-bit period to wait for a clock transition
   float pulse_adj;        // PE: how much of the previous pulse's deviation to adjust this pulse by, 0 to 1
   //                         NRZI: how much of the actual transition avg position to use to adjust the next expected
   float pkww_bitfrac;     // what fraction of the bit spacing the window width is
   float pkww_rise;        // the required rise in volts in the window that represents a peak (will be adjusted by AGC and peak height)
   float z1pt;             // GCR: fraction of a bit time that means one zero bit
   float z2pt;             // GCR: fraction of a bit time that means two zero bits
   // ...add more dynamic parameters above here, and in the arrays at the top of decoder.c
   char id[4];             // "PRM", to make sure the structure initialization isn't screwed up
   int tried;              // how many times this parmset was tried
   int chosen;             // how many times this parmset was chosen
}; // array: parmsets_xxx[block.parmset]

extern struct parms_t *parmsetsptr;    // pointer to the parmset we are using
#define PARM parmsetsptr[block.parmset]  // macro for referencing a current parameter
extern char *parmnames[];

enum bstate_t { // the decoding status a block
   BS_NONE,          // no status available yet
   BS_TAPEMARK,      // a tape mark
   BS_BLOCK,         // a good block, but maybe parity errors or faked bits
   BS_MALFORMED,     // a malformed block: different tracks are different lengths
   BS_ABORTED };     // we aborted processing for some reason

struct blkstate_t {  // state of the block, when we're done
   int tries;           // how many times we tried to decode this block
   int parmset;         // which parameter set we are currently using
   bool window_set;
   struct results_t {      // the results from the various parameter sets, in order
      enum bstate_t blktype;     // the ending state of the block
      int minbits, maxbits;      // the min/max bits of all the tracks
      float avg_bit_spacing;     // what the average bit spacing was, in secs
      int missed_midbits;        // how many times transitions were recognized after the midbit
      int vparity_errs;          // how many vertical (byte) parity errors it has
      int faked_bits;            // how many faked bits it has
      int errcount;              // how many total errors it has: parity + CRC + LRC
      int warncount;             // how many warnings it has: NRZI midbit errors, etc.
      bool crc_bad, lrc_bad;     // NRZI 800: are crc/lrc ok?
      int crc, lrc;              // NRZI 800; the actual crc anc lrc values in the data
      float alltrk_max_agc_gain; // the maximum AGC gain we used for any track
   } results [MAXPARMSETS]; // results for each parmset we tried
}; // block

void fatal(const char *, ...);
void assert(bool, const char *, ...); // Don't try to make into a macro -- too many problems!
void rlog(const char *, ...);
void debuglog(const char* msg, ...);
void breakpoint(void);
char *intcommas(int);
char *longlongcommas(long long int);
byte parity(uint16_t);
void trace_event(enum trace_names_t tracenum, double time, float tickdirection, struct trkstate_t *t);
void show_track_datacounts(char *);
void init_trackstate(void);
void init_blockstate(void);
void adjust_clock(struct clkavg_t *c, float delta, int trk);
void force_clock(struct clkavg_t *c, float delta, int trk);
void adjust_agc(struct trkstate_t *t);
void record_peakstat(float bitspacing, double peaktime, int trknum);
enum bstate_t process_sample(struct sample_t *);
void gcr_top(struct trkstate_t *t);
void gcr_bot(struct trkstate_t *t);
void gcr_midbit(struct trkstate_t *t);
void gcr_end_of_block(void);
void gcr_write_ecc_data(void);
void nrzi_top(struct trkstate_t *t);
void nrzi_bot(struct trkstate_t *t);
void nrzi_midbit(void);
void nrzi_end_of_block(void);
void pe_top(struct trkstate_t *t);
void pe_bot(struct trkstate_t *t);
void pe_generate_fake_bits(struct trkstate_t *t);
void pe_end_of_block(void);
void show_block_errs(int);
void output_peakstats(void);
bool parse_option(char *);
void skip_blanks(char **pptr);
bool getchars_to_blank(char **pptr, char *dstptr);
char *modename(void);
void skew_display(void);
void skew_set_delay(int trknum, float time);
void skew_set_deskew(void);
int skew_min_transitions(void);
void estden_init(void);
void estden_setdensity(int numblks);
void estden_show(void);
//bool estden_numtransitions(void);
bool estden_done(void);
bool ibm_label(void);
void create_datafile(const char *name);
void close_file(void);
void read_parms(void);
void txtfile_open(void); 
void txtfile_outputrecord(void);
void txtfile_tapemark(void);
void txtfile_close(void);

extern enum mode_t mode;
extern bool terse, verbose, quiet, multiple_tries, tap_format;
extern bool deskew, doing_deskew, doing_density_detection;
extern bool trace_on, trace_start;
extern bool hdr1_label;
byte expected_parity;
extern int dlog_lines;
extern double timenow, torigin;
extern int64_t timenow_ns;
extern float sample_deltat;
extern int64_t sample_deltat_ns;
extern double interblock_expiration;
extern struct blkstate_t block;
extern struct trkstate_t trkstate[MAXTRKS];
extern uint16_t data[], data_faked[];
extern double data_time[];
extern struct nrzi_t nrzi;
extern float bpi, ips;
extern int ntrks, num_trks_idle, numblks, numfiles, pkww_width;
extern char baseoutfilename[], baseinfilename[];

// must match arrays in textfile.c
enum txtfile_numtype_t { NONUM, HEX, OCT };
enum txtfile_chartype_t { NOCHAR, BCD, EBC, ASC, BUR };

extern enum txtfile_numtype_t txtfile_numtype;
extern enum txtfile_chartype_t txtfile_chartype;
extern int txtfile_linesize;
extern bool txtfile_doboth;

//*
