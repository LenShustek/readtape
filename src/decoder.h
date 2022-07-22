// file: decoder.h
/******************************************************************************

          header file for all readtape c files

   In addition to defining the common data structures,
   this also controls whether various debugging modes are enabled.

---> See readtape.c for the merged change log.

*******************************************************************************
Copyright (C) 2018,2019 Len Shustek

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
typedef unsigned char bool; // we don't use stdbool.h so we can have "unknown" as a value
#define true 1
#define false 0
#define unknown 0xff

#define DEBUG false                // generate debugging output?
#define DEBUGALL true              // for track debugging when trace is on: for all tracks?
#define TRACETRK 6                 // if not, which track gets special attention?

#define TRACEFILE (true & DEBUG)   // if DEBUG, are we also creating trace file?
#define TRACEALL true              // are we plotting all analog waveforms? Otherwise just one, TRACETRK
#define TRACESCALE 1.f //2.f             // scaling factor for voltages on the trace graph

#define PEAK_STATS true             // accumulate peak timing statistics?
#define DESKEW (true & PEAK_STATS)  // also add code for optional track deskewing?
#define DESKEW_PEAKDIFF_WARNING 0.10   // fraction of a bit to warn about deskewed peaks too far apart
#define DESKEW_STDDEV_WARNING 0.03     // fraction of a bit to warn about largest peak std deviation too big
#define CORRECT true                // add code to do data correction if -correct?
#define GCR_PARMSCAN false          // scan for optimal GCR parameters?
#define SHOW_TAP_OFFSET true        // show .tap file offsetof the block in the log
#define SHOW_NUMSAMPLES false       // show number of samples at this block in the log
#define SHOW_START_TIME false       // show start time of block in the log

#define DLOG_LINE_LIMIT 20000     // limit for debugging output

#if DEBUG
#define dlog(...) {debuglog(__VA_ARGS__);}
#define dlogtrk(...) {if(trace_on && (DEBUGALL || t->trknum == TRACETRK)) debuglog(__VA_ARGS__);}
#else
#define dlog(...) {}
#define dlogtrk(...) {}
#endif

#define TRACE(var,time,tickdirection,t) {if(TRACEFILE) trace_event(trace_##var, time, tickdirection, t);}
enum trace_names_t { //** MUST MATCH tracevals in decoder.c !!!
   trace_peak, trace_data, trace_avgpos, trace_zerpos, trace_adjpos, trace_zerchk,
   trace_parerr, trace_clkedg, trace_datedg, trace_clkwin, trace_clkdet };
#define UPTICK 0.75f
#define DNTICK -0.75f

#define _FILE_OFFSET_BITS 64     // on Linux, make file offsets be 64 bits
#include <stdio.h>
#if defined(_WIN32)              // there is NO WAY to do this in an OS-independent fashion!
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
#include <math.h>
typedef unsigned char byte;
#include "csvtbin.h"

#define MINTRKS 5
#define MAXBLOCK 131072
#define MAXPARMSETS 15
#define MAXPARMS 15
#define MAXPATH 300
#define MAXLINE 400

#define MAXSKEWSAMP 50     // maximum track skew amount in number of samples
#define MAXSKEWBLKS 100    // maximum blocks to preprocess to calibrate skew
#define MINSKEWTRANS 1000  // the minumum number of transitions we would like to base skew calibration on

// Here are lots of of parameters that control the decoding algorithm.
// Some of these are defaults or maxima, for which the currently used values are in the parms_t structure.
// More values can be moved into the parms_t structure if they need to change from tape to tape.

#define NRZI_IBG_SECS    200e-6     // minimum interblock gap in seconds (should depend on current IPS?)
#define NRZI_MIN_BLOCK   10         // minimum block size in bits
#define NRZI_MAX_MISMATCH 10        // maximum mismatch between tracks, in bits, for us to try to decode
#define NRZI_RESET_SPEED false      // should we reset the tape speed based on the second peak of a block?
#define NRZI_BADTRK_FACTOR  2.0     // how much worse the AGC of the worst track needs to be for correction

#define GCR_IDLE_THRESH  6.00       // more than these bit times without a peak means the track is idle
//                                     (must take into account the delay in peak or zerocross detection)
#define GCR_IBG_SECS     200e-6     // minimum interblock gap in seconds (should depend on current IPS?)

#define PE_IDLE_FACTOR     2.5f     // how much of the bit spacing to wait for before considering the track idle
#define PE_IBG_SECS        200e-6   // minimum interblock gap in seconds (should depend on current IPS?
#define PE_IGNORE_POSTBITS 5        // how many postamble bits to ignore
#define PE_MIN_PREBITS     70       // minimum number of peaks (half that number of bits) for a preamble
#define PE_MAX_POSTBITS    40       // maximum number of postamble bits we will remove

enum wwtrk_t { // Whirlwind track types
   WWTRK_PRICLK, WWTRK_PRILSB, WWTRK_PRIMSB,  // primary clock, LSB, and MSB
   WWTRK_ALTCLK, WWTRK_ALTLSB, WWTRK_ALTMSB,  // alternate clock, LSB, and MSB
   WWTRK_NUMTYPES };
#define WWTRKTYPE_SYMBOLS "CLMclmx"  // symbols we use to represent those track types, and "ignore"
#define WWHEAD_IGNORE (MAXTRKS-1)    // the track where we dump the unused head data
#define WW_CLKSTOP_BITS 1.5f         // after how many bits do we declare the clock stopped?
#define WW_PEAKSCLOSE_BITS 0.5f      // peaks closer than this are deemed to be for the same bit
#define WW_PEAKSFAR_BITS 2.0f        // peaks farther than this are deemed to be unrelated
#define WW_MAX_CLK_VARIATION 0.10f   // percentage of clock speed variation above which to complain about

#define PKWW_MAX_WIDTH   50         // the peak-detect moving window maximum width, in number of samples
#define PKWW_PEAKHEIGHT  4.0f       // the assumed peak-to-peak (2x top or bot height) voltage for the pkww_rise parameter

#define DIFFERENTIATE_THRESHOLD 0.05f   // differentiation delta must be greater than this, otherwise use 0
#define DIFFERENTIATE_SCALE 0.4f        // volts per sample to scale sample delta

#define ZEROCROSS_PEAK   0.2f       // for zerocrossing, the minimum peak excursion before we consider a zero crossing
#define ZEROCROSS_SLOPE  1.5f       // the maximum time, in bits, for the required peak to be attained after a zero crossing

#define PEAK_THRESHOLD   0.005f     // volts of difference that define "same peak", scaled by AGC
#define CLKRATE_WINDOW   50         // maximum window width for clock averaging
#define FAKE_BITS        true       // should we fake bits during a dropout?
#define USE_ALL_PARMSETS false      // should we try to use all the parameter sets, to be able to rate them?

#define SKIP_NOISE       true       // should we skip a noise block whenever we decode one, or try with alternate decodings?
// The upside of SKIP_NOISE is that true noise before a low-peak block won't cause a higher-threshold parmset to be tried and
// absorb the block even though it has errors, and the low-threshold parmset would have decoded it perfectly.
// The downside is that a block starting with high peaks followed by very low peaks will be seen as noise by an initial
// parmset that has a high threshold. So use SKIP_NOISE but maybe put high-threshold parmsets later in the list?

#define AGC_MAX_WINDOW   10         // maximum number of peaks to look back for the min peak
#define AGC_MAX_VALUE     2.0f      // maximum value of AGC
#define AGC_STARTBASE     5         // starting peak for baseline voltage measurement
#define AGC_ENDBASE      15         // ending peak for baseline voltage measurement

#define NULLP (void *)0
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
//Brian Kernighan's clever method of counting one bits by iteratively clearing the least significant bit set
#define COUNTBITS(ctr, word) for (ctr = 0; word; ++ctr) word &= word - 1;

#define TICK(x) ((x - torigin) / sample_deltat - 1)
#define TIMETICK(x) x,((x - torigin) / sample_deltat - 1)
#define TIMEFMT "%.8lf tick %.1lf"

// verbose (-v...) flags
#define VL_BLKSTATUS 0x01          // summary block-by-block status (devault for -v)
#define VL_WARNING_DETAIL 0x02     // details about all block warnings
#define VL_ATTEMPTS 0x04           // show each block decode attempt
#define VL_TRACKLENGTHS 0x08       // show block track mismatch lengths

// debug (-d...) flags, only if DEBUG on
#define DB_BLKSTATUS 0x01          // show block parmset choice progress (default for -d)
#define DB_GCRERRS 0x02            // show GCR bad dgroups and parity errors
#define DB_PEAKS 0x04              // show waveforms peaks and zero-crossings; 0/1 bits added, pulse position adjustments

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
   float v_last_raw;       // last raw voltage, before differentiating
   float v_now;            // current voltage
   float v_prev;           // previous voltage

   float v_top;            // top voltage
   double t_top;           // time of top peak, or of upward zero crossing
   float v_lasttop;        // remembered last top voltage

   float v_bot;            // bottom voltage
   double t_bot;           // time of bottom peak, or of downward zero crossing
   float v_lastbot;        // remembered last bottom voltage
   double t_lastbot;       // remember last bottom time

   float v_lastpeak;       // last peak (top or bottom) voltage
   double t_lastpeak;      // time of last top or bottom or zero crossing
   double t_prevlastpeak;  // time of the one before that
   bool zerocross_up_pending; // we have a potential zero crossing up pending at t_top
   bool zerocross_dn_pending; // we have a potential zero crossing down pending at t_bot
   double t_firstzero;        // the time of the first zero
   double t_lastzero;         // the time of the last zero
   float t_peakdelta;      // GCR: delta time between most recent peaks or zero crossings
   float t_peakdeltaprev;  // GCR: the previous delta time between peaks or zero crossings
   double t_lastpulsestart; // Whirlwind: the last time we saw a flux transition for pulse start
   double t_lastpulseend;   // Whirlwind: the last time we saw a flux transition for pulse end

   float pkww_v[PKWW_MAX_WIDTH];  // the window of sample voltages
   float pkww_minv;        // the minimum voltage in the window
   float pkww_maxv;        // the maximum voltage in in the window
   int pkww_left;          // the index into the left edge sample
   int pkww_right;         // the index into the right edge sample
   int pkww_countdown;     // countdown timer for peak to exit the window

   float v_avg_height;     // average of peak-to-peak voltage during preamble or deskew calculation
   float v_avg_height_sum; // temp for summing the initial average
   int v_avg_height_count; // how many samples went into that average
   float agc_gain;         // the current AGC gain, based on recent peaks
   float max_agc_gain;     // the highest ever AGC gain for this track on this block
   float min_agc_gain;     // the lowest ever AGC gain for this track on this block
   float v_heights[AGC_MAX_WINDOW];  // last n peak-to-peak voltages
   int heightndx;                // index into v_heights of next spot to use

   double t_lastbit;       // time of last data bit transition
   double t_firstbit;      // time of first data bit transition in the data block
   double t_lastclock;     // GCR: the time of the last clock
   int consecutive_zeroes; // GCR: how many consecutive all-track zeroes we have seen
   float t_clkwindow;      // PE: how late, in usec, a clock transition can be before we consider it data
   float t_pulse_adj;      // PE: how much, in usec, to adjust the pulse by based on previous pulse's timing
   bool bit1_up;           // PE: is a 1-bit up or down?
   struct clkavg_t clkavg; // PE and GCR: data for computing average clock
   int datacount;          // how many data bits we've seen
   int peakcount;          // how many peaks (flux reversals) we've seen
   byte lastdatabit;       // the last data bit we recorded
   bool idle;              // are we idle, ie not seeing transitions?
   bool clknext;           // PE: do we expect a clock next?
// bool hadbit;            // NRZI: did we have a bit transition since the last check?
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

struct nrzi_t { // NRZI decode state information
   double t_lastclock;     // time of the last clock
   double t_last_midbit;   // the last mid-bit boundary we checked for zeroes
   struct clkavg_t clkavg; // the current bit rate estimate
   bool datablock;         // are we in a data block?
   bool reset_speed;       // did we reset the speed ourselves?
   int post_counter;       // counter for post-data bit times: CRC is at 3-4, LRC is at 7-8
}; // nrzi.

struct ww_t { // Whirlwind decode state information
   struct clkavg_t clkavg; // the current bit rate estimate
   bool datablock;         // are we in a data block?
   int datacount;          // the count of 2-bit nibbles
   double t_lastpeak;             // the time of the last peak on any track
   double t_lastclkpulsestart;    // the last (first to occur) clock pulse start time
   double t_lastclkpulseend;      // the last (first to occur) clock pulse end time
   double t_lastpriclkpulsestart; // the last primary clock pulse start
   double t_lastaltclkpulsestart; // the last alternate clock pulse start
   double t_lastpriclkpulseend;   // the last primary clock pulse end time, used for skew calculations of the other tracks
   double t_lastblockmark; // the time of the last blockmark
   bool blockmark_queued;  // we have a blockmark queued up to return
}; // ww

#define MAXPARMCOMMENT 80
struct parms_t {  // a set of parameters used for decoding a block. We try again with different sets if we get errors.
   int active;             // 1 means this is an active parameter set
   int clk_window;         // how many bit times to average for clock rate; 0 means maybe use exponential averaging
   float clk_alpha;        // weighting for current data in the clock rate exponential weighted average; 0 means use constant
   int agc_window;         // how many peaks to look back for the min peak to set AGC; 0 means maybe use exponential averaging
   float agc_alpha;        // weighting for current data in the AGC exponential weight average; 0 means no AGC
   float min_peak;         // peak detection: the minimum height of a peak in volts, above or below 0 volts (not relative!)
   float clk_factor;       // PE: how much of a half-bit period to wait for a clock transition
   float pulse_adj;        // PE: how much of the previous pulse's deviation to adjust this pulse by, 0 to 1
   //                         NRZI: how much of the actual transition avg position to use to adjust the next expected
   float pkww_bitfrac;     // what fraction of the bit spacing the window width is
   float pkww_rise;        // the required rise in volts in the window that represents a peak (will be adjusted by AGC and peak height)
   float midbit;           // NRZI: what fraction of a bit time is the midbit point for determining zeroes
   float z1pt;             // GCR: fraction of a bit time that means one zero bit
   float z2pt;             // GCR: fraction of a bit time that means two zero bits
   // ...add more dynamic parameters above here, and in the arrays at the top of decoder.c
   char id[4];             // "PRM", to make sure the structure initialization isn't screwed up
   char comment[MAXPARMCOMMENT]; // saved comment for this parmset
   int tried;              // how many times this parmset was tried
   int chosen;             // how many times this parmset was chosen
}; // array: parmsets_xxx[block.parmset]

extern struct parms_t *parmsetsptr;    // pointer to the parmset we are using
#define PARM parmsetsptr[block.parmset]  // macro for referencing a current parameter
extern char *parmnames[];

enum flux_direction_t { FLUX_POS, FLUX_NEG, FLUX_AUTO }; // currently only for Whirlwind

enum bstate_t { // the decoding status a block
   // must agree with bs_name[] in readtape.c
   BS_NONE,          // no status is available yet
   BS_TAPEMARK,      // a tape mark
   BS_NOISE,         // a block so short as to be noise
   BS_BADBLOCK,      // a block so bad we can't write it
   BS_BLOCK,         // a block we can write, but maybe with errors or warnings
   BS_ABORTED };     // we aborted processing for some reason

struct blkstate_t {  // state of the block, when we're done
   int tries;           // how many times we tried to decode this block
   int parmset;         // which parameter set we are currently using
   bool window_set;     // have we decided on the peak-detection window size?
   bool endblock_done;  // have we done end-of-block processing?
   double t_blockstart; // the time when the block started
   struct results_t {      // the results from the various parameter sets, in order
      enum bstate_t blktype;     // the ending state of the block
      int minbits, maxbits;      // the min/max bits of all the tracks
      float avg_bit_spacing;     // what the average bit spacing was, in secs
      int warncount;             // how many warnings it has:
      int missed_midbits;        //    how many times transitions were recognized after the midbit
      int corrected_bits;        //    how many correct (or ec) bits we generated
      int gcr_bad_dgroups;       //    GCR: how many bad dgroups we found and guessed about
      int ww_leading_clock;      //    WW: the block had one spurious leading clock bit (length mod 8 = 1)
      int ww_missing_onebit;     //    WW: instances where a 1-bit was only on one of the data tracks
      int ww_missing_clock;      //    WW: instances where a clock only appeared on one of the clock tracks
      uint16_t faked_tracks;     // which tracks had corrected bits
      int errcount;              // how many total errors it has:
      int track_mismatch;        //    how badly the track lengths are mismatched
      int vparity_errs;          //    how many vertical (byte) parity errors it has
      int ecc_errs;              //    GCR: how many ECC errors it has
      int crc_errs;              //    GCR, 9-track NRZI: how many CRC errors it has
      int lrc_errs;              //    NRZI: how many LRC errors it has
      int gcr_bad_sequence;      //    GCR: how many sgroup sequence errors we found
      int ww_bad_length;         //    WW: the block had bad length (mod 8 isn't 0 or 1)
      int ww_speed_err;          //    WW: the clock speed got out of whack
      int first_error;           // GCR: the datacount where we found the first error in the block
      int crc, lrc;              // NRZI 800; the actual crc anc lrc values in the data
      float alltrk_max_agc_gain; // the maximum AGC gain we used for any track
      float alltrk_min_agc_gain; // the minumum AGC gain we used for any track
   } results [MAXPARMSETS]; // results for each parmset we tried
}; // block

void fatal(const char *, ...);
void assert(bool, const char *, ...); // Don't try to make into a macro -- too many problems!
void rlog(const char *, ...);
void debuglog(const char* msg, ...);
void breakpoint(void);
char *intcommas(int);
char *longlongcommas(long long int);
char const *add_s(int value);
byte parity(uint16_t);
void trace_event(enum trace_names_t tracenum, double time, float tickdirection, struct trkstate_t *t);
void trace_close(void);
void show_track_datacounts(char *);
void init_trackstate(void);
void init_trackpeak_state(void);
void init_blockstate(void);
void set_expected_parity(int blklength);
void adjust_clock(struct clkavg_t *c, float delta, int trk);
void force_clock(struct clkavg_t *c, float delta, int trk);
void adjust_agc(struct trkstate_t *t);
void accumulate_avg_height(struct trkstate_t *t);
void compute_avg_height(struct trkstate_t *t);
void record_peakstat(float bitspacing, float peaktime, int trknum);
void adjust_deskew(float bitspacing);
enum bstate_t process_sample(struct sample_t *);
void gcr_top(struct trkstate_t *t);
void gcr_bot(struct trkstate_t *t);
void gcr_end_of_block(void);
void gcr_preprocess(void);
void gcr_write_ecc_data(void);
void nrzi_top(struct trkstate_t *t);
void nrzi_bot(struct trkstate_t *t);
void nrzi_zerocheck(void);
void nrzi_end_of_block(void);
void pe_top(struct trkstate_t *t);
void pe_bot(struct trkstate_t *t);
void pe_generate_fake_bits(struct trkstate_t *t);
void pe_end_of_block(void);
void ww_top(struct trkstate_t *t);
void ww_bot(struct trkstate_t *t);
void ww_end_of_block(void);
void ww_init_blockstate(void); 
void init_clkavg(struct clkavg_t *c, float init_avg); 
void ww_blockmark(void);
void show_block_errs(int);
void output_peakstats(const char *name);
bool parse_option(char *);
void skip_blanks(char **pptr);
bool getchars_to_blank(char **pptr, char *dstptr);
char *modename(void);
void skew_display(void);
bool skew_compute_deskew(bool do_set);
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
void txtfile_outputrecord(int length, int numerrs, int numwarnings);
void txtfile_tapemark(bool tapfile);
void txtfile_message(const char *msg,...);
void txtfile_close(void);
char * format_block_errors(struct results_t *result);
void read_tapfile(const char *basefilename, const char *extension);

extern enum mode_t mode;
extern enum wwtrk_t ww_trk_to_type[MAXTRKS];
extern int ww_type_to_trk[WWTRK_NUMTYPES];
extern bool verbose, quiet, multiple_tries, tap_format, tap_read, do_correction, do_differentiate, labels;
extern bool deskew, adjdeskew, doing_deskew, skew_given, doing_density_detection, find_zeros;
extern bool trace_on, trace_start;
extern bool hdr1_label, reverse_tape, invert_data, autoinvert_data, txtfile_verbose;
extern byte expected_parity, specified_parity;
extern int revparity;
extern enum flux_direction_t flux_direction_requested, flux_direction_current;
extern int dlog_lines, verbose_level, debug_level;
extern double timenow, torigin;
extern int64_t timenow_ns;
extern float sample_deltat;
extern int64_t sample_deltat_ns;
extern int interblock_counter;
extern struct blkstate_t block;
extern struct trkstate_t trkstate[MAXTRKS];
extern int skew_delaycnt[MAXTRKS];
extern float deskew_max_delay_percent;
extern uint16_t data[], data_faked[];
extern double data_time[];
extern struct nrzi_t nrzi;
extern struct ww_t ww;
extern float bpi, ips;
extern int ntrks, num_trks_idle, numblks, numfiles, num_flux_polarity_changes, pkww_width;
extern char baseoutfilename[], baseinfilename[];
extern char version_string[];

// must match arrays in textfile.c
enum txtfile_numtype_t { NONUM, HEX, OCT, OCT2 };
enum txtfile_chartype_t { NOCHAR, BCD, EBC, ASC, BUR, SIXBIT, SDS, SDSM, FLEXO, ADAGE, ADAGETAPE, CDC, UNIVAC };

extern enum txtfile_numtype_t txtfile_numtype;
extern enum txtfile_chartype_t txtfile_chartype;
extern int txtfile_linesize, txtfile_dataspace;
extern bool txtfile_doboth, txtfile_linefeed;

void trace_newtime(double time, float deltat, struct sample_t *sample, struct trkstate_t *t);
void trace_startstop(void);

//*
