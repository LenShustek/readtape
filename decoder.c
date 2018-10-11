//file: decoder.c
/*****************************************************************************

Decode analog magnetic tape data in one of several flavors:
  -- Manchester phase encoded (PE)
  -- NRZI (non-return-to-zero inverted) encoded
  -- GCR group coded recording

We are called once for each set of read head voltages on 9 data tracks.

Each track is processed independently, so that head and data skew
(especially for PE and GCR, which are self-clocking) is irrelevant.
We look for relative minima and maxima of the head voltage that
represent the downward and upward flux transitions.

For PE we use the timing of the transitions to reconstruct the original
data that created the Manchester encoding: a downward flux transition
for 0, an upward flux transition for 1, and clock transitions as needed
in the midpoint between the bits.

For NRZI we recognize either transition as a 1 bit, and the lack of
a transition at the expected bit time as a 0 bit. With vertical odd
parity for 7- and 9-track tapes, at least one track will have a
transition at each bit time. We do need to compensate for track skew,
since the data recovery for all tracks is done together. There is an
optional prescan phase to compute the skew.

For GCR, even though it is NRZI we treat each track independently as
for PE, since the coding guarantees that there are no more than two
consecutive zeroes. That means we don't have to worry about track skew.

We dynamically track the bit timing as it changes, to cover for
variations in tape speed. We can work with any average tape speed,
as long as Faraday's Law produces an analog signal enough above
the noise. We are also independent of the number of analog samples
per flux transition, although having more than 10 is good.

We compute the natural peak-to-peak amplitude of the signal from
each head by sampling a few dozen bits at the start of a block.
Then during the data part of the block, we increase the simulated
gain when the peak-to-peak amplitude decreases. This AGC (automatic
gain control) works well for partial dropouts during a block.

If we see a total dropout in a PE track, we wait for data to
return. Then we create "faked" bits to cover for the dropout, and
keep a record of which bits have been faked. Unfortunately this
doesn't work as well for NRZI, since the dropout might be in the
only track that has transitions and we would have to estimate the
clock by dead-reckoning.

The major routine here is process_sample(). It returns an indication
of whether the current sample represents the end of a block, whose
state is then in struct blkstate_t block.

----> See readtape.c for the merged change log.

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

#include "decoder.h"

// Variables for time relative to the start must be at least double-precision.
// Delta times for samples close to each other can be single-precision,
// although the compiler might complain about loss of precision.
// Time calculations that are subject to cumulative errors must be done
// in 64-bit integer number of nanoseconds.

int num_trks_idle;
int num_samples;
int errcode=0;
struct nrzi_t nrzi = { 0 };      // NRZI decoding status
int pkww_width = 0;              // the width of the peak detection window, in number of samples

double timenow = 0;              // time of last sample in seconds
int64_t timenow_ns = 0;          // time of last sample in nanoseconds (used when reading .tbin files)
double torigin = 0;              // time origin for debugging displays, in seconds
float sample_deltat = 0;         // time between samples, in seconds
int64_t sample_deltat_ns = 0;    // time between samples, in nanoseconds
double interblock_expiration = 0;// interblock gap expiration time

struct trkstate_t trkstate[MAXTRKS] = { 0 };// the current state of all tracks

uint16_t data[MAXBLOCK+1] = { 0 };		     // the reconstructed data in bits 8..0 for tracks 0..7, then P as the LSB
uint16_t data_faked[MAXBLOCK+1] = { 0 };    // flag for "data was faked" in bits 8..0 for tracks 0..7, then P as the LSB
double data_time[MAXBLOCK+1] = { 0 };	     // the time the last track contributed to this data byte

struct blkstate_t block;  // the status of the current data block as decoded with the various sets of parameters

/***********************************************************************************************************************
Routines for accumulating delta flux transition times.

For NRZI, there is one peak for each track because at least one track always has a transition at the bit time,
and we show the peaks relative to that. We also output the average in a separate column that shouldn't be graphed.

For PE there are two peaks, corresponding to a one-bit (1/2 bit time interval) or zero-bit (1 bit time interval)

For GCR, where the tracks are skewed relative to each other and decoded independently, each track has three peaks
corresponding to an interval of 0, 1, or 2 zero bits between 1-bit flux transitions. The 5-bit GCR coding
guarantees no more than 2 consecutive zero bits.

************************************************************************************************************************/
#if PEAK_STATS
#define PEAK_STATS_NUMBUCKETS 50
float peak_stats_leftbin = 0;
float peak_stats_binwidth = 0;
int peak_counts[MAXTRKS][PEAK_STATS_NUMBUCKETS] = { 0 };
int peak_trksums[MAXTRKS] = { 0 };

void record_peakstat(float bitspacing, double peaktime, int trknum) {
   static bool initialized = false;
   if (!initialized) { // first time: set range for statistics
      // we want bins for a half bit time below the expected peak(s), and a half bit time above
      float range = bitspacing *
                    (mode == NRZI ? 1.0f
                     : mode == PE ? 1.2f
                     : /* GCR */ 3.0f);
      peak_stats_binwidth = range / PEAK_STATS_NUMBUCKETS;
      // round to the nearest 0.1 usec
      peak_stats_binwidth = (float)((int)(peak_stats_binwidth*10e6+0.5)*1e-6)/10.0f;
      peak_stats_leftbin = mode == PE ? bitspacing * 0.25f : bitspacing * 0.5f;
      // round to next lower multiple of BINWIDTH
      peak_stats_leftbin = (float)(int)(peak_stats_leftbin /peak_stats_binwidth) * peak_stats_binwidth;
      dlog("peakstats started: bitspacing %.2f, peaktime %.7lf range %.2f binwidth %.2f leftbin %.2f\n",
           bitspacing*1e6, peaktime, range*1e6, peak_stats_binwidth*1e6, peak_stats_leftbin*1e6);
      initialized = true; }
   int bucket = (int)((peaktime - peak_stats_leftbin) / peak_stats_binwidth);
   bucket = max(0, min(bucket, PEAK_STATS_NUMBUCKETS - 1));
   ++peak_counts[trknum][bucket];
   ++peak_trksums[trknum]; }

void output_peakstats(void) { // Create an Excel .CSV file with flux transition position statistics
   FILE *statsf;
   char filename[MAXPATH];
   int trk, bkt;
   int totalcount = 0;
   long long int avgsum;
   sprintf(filename, "%s.peakstats.csv", baseoutfilename);
   assert((statsf = fopen(filename, "w")) != NULLP, "can't open stats file \"%s\"", filename);
   for (bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt)
      fprintf(statsf, ",%.1f uS", peak_stats_binwidth*1e6 * bkt + peak_stats_leftbin * 1e6);
   if (mode == NRZI) fprintf(statsf, ",avg uS");
   fprintf(statsf, "\n");
   for (trk = 0; trk < ntrks; ++trk) {
      for (avgsum = bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt) {
         avgsum += (long long)(peak_counts[trk][bkt] * (peak_stats_binwidth*1e6 * bkt + peak_stats_leftbin * 1e6)); }
      fprintf(statsf, "trk%d,", trk);
      for (bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt)
         fprintf(statsf, "%.2f%%, ", 100 * (float)peak_counts[trk][bkt] / (float)peak_trksums[trk]);
      if (mode == NRZI) fprintf(statsf, "%.2f", (float)avgsum / (float)peak_trksums[trk]);
      fprintf(statsf, "\n");
      totalcount += peak_trksums[trk]; }
   fclose(statsf);
   if (!quiet) rlog("created statistics file \"%s\" from %s measurements of flux transition positions\n", filename, intcommas(totalcount)); }

#endif

/***********************************************************************************************************************
   Routines for using accumulated flux transition times to
   deskew the input data by delaying some of the channel data

We do this only for NRZI, where the tracks must be aligned.
PE and GCR are self-clocking, so track skew isn't a problem.
************************************************************************************************************************/

#if DESKEW
struct skew_t {
   float vdelayed[MAXSKEWSAMP]; // the buffered voltages
   int ndx_next;            // the next slot to use for the newest data
} skew[MAXTRKS];            // (This structure is cleared at the start of each block.)
int skew_delaycnt[MAXTRKS] = { 0 };    // the skew delay, in number of samples, for each track. (This is persistent.)

void skew_set_delay(int trknum, float time) { // set the skew delay for a track
   assert(sample_deltat > 0, "delta T not set yet in skew_set_delay");
   assert(time >= 0, "negative skew amount %f", time);
   int delay = (int)((time + sample_deltat/2) / sample_deltat);
   //rlog("set skew trk %d to %f usec, %d samples\n", trknum, time*1e6, delay);
   if (delay > MAXSKEWSAMP) rlog("track %d skew of %.1f usec is too big\n", trknum, time*1e6);
   skew_delaycnt[trknum] = min(delay, MAXSKEWSAMP); };

void skew_set_deskew(void) {  // set all the deskew amounts based on where we see most of the transitions
   int trk, bkt;
   long long int avgsum;
   float avg[MAXTRKS]; // the average peak position for each track
   for (trk = 0; trk < ntrks; ++trk) {
      for (avgsum = bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt) {
         avgsum += (long long) (peak_counts[trk][bkt] * (peak_stats_binwidth*1e6 * bkt + peak_stats_leftbin * 1e6)); }
      avg[trk] = (float)avgsum / (float)peak_trksums[trk]; }
   float maxavg = 0;
   for (trk = 0; trk < ntrks; ++trk) // see which track has the last transitions, on average
      maxavg = max(maxavg, avg[trk]);
   for (trk = 0; trk < ntrks; ++trk) {
      //rlog("trk %d has %d transitions, avg %f max %f\n", trk, peak_trksums[trk], avg[trk], maxavg);
      skew_set_delay(trk, peak_trksums[trk] > 0 ? (maxavg - avg[trk]) / 1e6f : 0); // delay the other tracks relative to it
   }
   if (!quiet) skew_display(); }

int skew_min_transitions(void) { // return the lowest number of transitions on any track
   int min_transitions = INT_MAX;
   for (int trknum = 0; trknum < ntrks; ++trknum)
      if (peak_trksums[trknum] < min_transitions) min_transitions = peak_trksums[trknum];
   return min_transitions; }

void skew_display(void) { // show the track skews
   for (int trknum = 0; trknum < ntrks; ++trknum)
      rlog("  track %d delayed by %d clocks (%.2f usec) based on %d observed flux transitions\n",
           trknum, skew_delaycnt[trknum], skew_delaycnt[trknum] * sample_deltat * 1e6, peak_trksums[trknum]); };
#endif

/***********************************************************************************************************************
   Routines for estimating the bit density on the tape based on
   the first several thousand samples of flux transition timing.
   We pick one of the standard tape densities if we are close.
************************************************************************************************************************/

#define ESTDEN_BINWIDTH 0.5e-6      // quantize transition delta times into bins of this width, in seconds
#define ESTDEN_MAXDELTA 120e-6      // ignore transition delta times bigger than this
#define ESTDEN_NUMBINS 150          // how many bins for different delta times we have
#define ESTDEN_COUNTNEEDED 9999     // how many transitions we need to see for a good estimate
#define ESTDEN_MINPERCENT 5         // the minimum transition delta time must be seen at least this many percent of the total
#define ESTDEN_CLOSEPERCENT 20      // how close, in percent, to one of the standard densities we need to be

struct {
   int deltas[ESTDEN_NUMBINS];   // distance between transitions, in seconds/ESTDEN_BINWIDTH
   int counts[ESTDEN_NUMBINS];   // how often we've seen that distance
   int binsused;
   int totalcount; } estden;

void estden_init(void) {
   memset(&estden, 0, sizeof(estden)); }

bool estden_done(void) {
   return estden.totalcount >= ESTDEN_COUNTNEEDED; }

bool estden_transition(float deltasecs) { // count a transition distance
   int delta = (int) (deltasecs / ESTDEN_BINWIDTH);  // round down to multiple of BINWIDTH
   int ndx;
   assert(deltasecs > 0, "negative delta %f usec in estden_transition", deltasecs*1e6);
   if (deltasecs <= ESTDEN_MAXDELTA) {
      for (ndx = 0; ndx < estden.binsused; ++ndx) // do we have it already?
         if (estden.deltas[ndx] == delta) break;  // yes
      if (ndx >= estden.binsused) { // otherwise create a new bucket
         assert(estden.binsused < ESTDEN_NUMBINS, "estden: too many transition delta values: %d", estden.binsused);
         estden.deltas[estden.binsused++] = delta; }
      ++estden.counts[ndx];
      ++estden.totalcount; }
   return estden_done(); }

void estden_show(void) {
   rlog("density estimation buckets, %.2f usec each:\n", ESTDEN_BINWIDTH*1e6);
   for (int ndx = 0; ndx < estden.binsused; ++ndx)
      rlog(" %2d: %5.1f usec cnt %d\n", ndx, estden.deltas[ndx] * ESTDEN_BINWIDTH * 1e6, estden.counts[ndx]); }

void estden_setdensity(int nblks) { // figure out the tape density
   int mindist = INT_MAX;
   // Look for the smallest transition distance that occurred at least 5% of the time.
   // That lets us ignore a few noise glitches and bizarre cases, should they occur.
   for (int ndx = 0; ndx < estden.binsused; ++ndx) {
      if (estden.counts[ndx] > estden.totalcount * ESTDEN_MINPERCENT / 100
            && estden.deltas[ndx] < mindist)
         mindist = estden.deltas[ndx]; }
   // estimate the density from that minimum transition delta time
   float density = 1.0f / (ips * (float)(mindist+0.5f) * (float)ESTDEN_BINWIDTH);
   if (mode == PE) density /= 2;  // twice the transitions for phase encoded data
   // search for a standard density that is close
   static float standard_densities[] = { 200, 556, 800, 1600, 9042 /* GCR at "6250" */, 0 };
   for (int ndx = 0; standard_densities[ndx]; ++ndx) {
      float stddensity = standard_densities[ndx];
      float diff = density - stddensity;
      if (diff < 0) diff = -diff;
      if (diff < stddensity * ESTDEN_CLOSEPERCENT / 100) {
         bpi = stddensity;
         if (!quiet) rlog("Set density to %.0f BPI after reading %d blocks and seeing %s transitions in %d bins that imply %.0f BPI\n",
                             bpi, nblks, intcommas(estden.totalcount), estden.binsused, density);
         return; } }
   fatal("The detected density of %.0f (%.1f usec) after seeing %s transitions is non-standard; please specify it.",
         density, (float)(mindist + 0.5) * ESTDEN_BINWIDTH * 1e6, intcommas(estden.totalcount)); }

/*****************************************************************************************************************************
   Routines for creating the trace file
******************************************************************************************************************************/
/* This stuff creates a CSV trace file with one or all tracks of voltage data
plus all sorts of debugging event info. To see the visual timeline, create a
line graph in Excel from column C through the next blank column.

The compiler switch that turns this on, and the track number to record special
infomation about, is at the top of decoder.h.
The start and end of the graph is controlled by code at the bottom of this file.

The trace data is buffered before being written to the file so that we can
"rewrite history" for events that are discovered late. That happens, for
example, because the new moving-window algorithm for peak detection finds
the peaks several clock ticks after they actually happen.
*/

bool trace_on = false, trace_done = false, trace_start = false;
int trace_lines = 0;
FILE *tracef;

#define TRACE_DEPTH 200

#define TB 3.00f   // base y-axis display level for miscellaneous stuff
#define TT -6.00f  // base y-axis display level for track info
#define TS 10.00f  // track separation, below zero on the y-axis
// also see: UPTICK, DOWNTICK in decoder.h

struct trace_val_t {  // the trace history buffer for an event
   char *name;          // what it's called
   enum mode_t mode;    // which encoding modes this is for
   int flags;           // any combination of T_xxxx flags
#define T_PERSISTENT 0x01  // show the value indicated by the last up or down transition, not the current value
#define T_SHOWTRK 0x02     // show this with the track at the baseline display level of the voltage
#define T_ONLYONE 0x04     // there is only one of these, not one per track
   float graphbase;     // the baseline output graph y-axis position
   float lastval;       // the last y-axis position output (for persistent values)
   float val[TRACE_DEPTH][MAXTRKS]; // all the events for all the tracks
}
tracevals[] = { //** MUST MATCH trace_names_t in decoder.h !!!
   { "peak",   ALLMODES, T_SHOWTRK,     0 },
   { "data",   ALLMODES, T_PERSISTENT, TB + 0.0 },
   { "avgpos", NRZI,     T_ONLYONE,    TB + 3*UPTICK },
   { "zerpos", GCR,      T_SHOWTRK,     0 + 4*UPTICK },
   { "adjpos", GCR,      T_SHOWTRK,     0 + 2*UPTICK },
   { "midbit", NRZI,     T_ONLYONE,    TB + 5*UPTICK },
   { "clkedg", PE,       0,            TB + 3*UPTICK },
   { "datedg", PE,       0,            TB + 5*UPTICK },
   { "clkwin", PE,       T_PERSISTENT, TB + 7*UPTICK },
   { "clkdet", PE,       T_PERSISTENT, TB + 9*UPTICK },
   { NULLP } };

struct {    // the trace history buffer for things other than events, which are various scalers
   double time_newest;        // the newest time in the buffer
   float deltat;              // the delta time from entry to entry
   double times[TRACE_DEPTH]; // the time of each slot
   float voltages[TRACE_DEPTH][MAXTRKS]; // the sampled voltage of each track
   int ndx_next;              // the next slot to use
   int num_entries;           // how many entries we put in
   // the remaining fields are "extra credit" data that we also write to the spreadsheet but aren't for graphing
   int datacount[TRACE_DEPTH];
   float agc_gain[TRACE_DEPTH];
   float bitspaceavg[TRACE_DEPTH];
   float t_peakdelta[TRACE_DEPTH]; //
}
traceblk = {0 };

void trace_dump(void) { // display the entire trace buffer
   rlog("trace buffer after %d entries, delta %.2f uS, next slot is %d\n",
        traceblk.num_entries, traceblk.deltat*1e6, traceblk.ndx_next);
   int ndx = traceblk.ndx_next;
   for (int i = 0; i < TRACE_DEPTH; ++i) {
      if (traceblk.times[ndx] != 0) {
         rlog("%2d: %.8lf, %5.2lf, ", ndx, traceblk.times[ndx], (traceblk.time_newest - traceblk.times[ndx])*1e6);
         for (int j = 0; tracevals[j].name != NULLP; ++j)
            rlog("%s:%5.2f, ", tracevals[j].name, tracevals[j].val[ndx]);
         rlog("\n"); }
      if (++ndx >= TRACE_DEPTH) ndx = 0; } };

void trace_writeline(int ndx) {  // write out one buffered trace line
   if (tracef) {
      fprintf(tracef, "%.8lf, ,", traceblk.times[ndx]);
      for (int trk = 0; trk < ntrks; ++trk) { // for all the tracks we're doing
         if (TRACEALL || trk == TRACETRK) {
            float level = TT - trk*TS; // base y-axis level for this track
            fprintf(tracef, "%.4f,",  // output voltage
                    traceblk.voltages[ndx][trk] * TRACESCALE + level);
            for (int i = 0; tracevals[i].name != NULLP; ++i) // do other associated columns
               if (tracevals[i].flags & T_SHOWTRK)
                  fprintf(tracef, "%.2f, ", tracevals[i].val[ndx][trk] + level); } }
      for (int i = 0; tracevals[i].name != NULLP; ++i) { // do the events not shown with the tracks
         if (tracevals[i].mode & mode) { // if we are the right mode
            if (!(tracevals[i].flags & T_PERSISTENT) ||
                  tracevals[i].val[ndx][TRACETRK] != tracevals[i].graphbase)
               tracevals[i].lastval = tracevals[i].val[ndx][TRACETRK];
            if ( !(tracevals[i].flags & T_SHOWTRK)) fprintf(tracef, "%f, ", tracevals[i].lastval); } }
      fprintf(tracef, ", %d, %.2f, %.2f, %.2f\n", // do the extra-credit stuff
              traceblk.datacount[ndx], traceblk.agc_gain[ndx], traceblk.bitspaceavg[ndx]*1e6, traceblk.t_peakdelta[ndx]*1e6); } }

void trace_newtime(double time, float deltat, struct sample_t *sample, struct trkstate_t *t) {
   // Create a new timestamped entry in the trace history buffer.
   // It's a circular list, and we write out the oldest entry to make room for the new one.
   if (tracef && trace_on) {
      //dlog("trace_newtime at %.7lf, delta %.2f, trk %d\n", time, deltat*1e6, t->trknum);
      traceblk.deltat = deltat;
      if (traceblk.num_entries++ >= TRACE_DEPTH)
         trace_writeline(traceblk.ndx_next);  // write out the oldest entry being evicted
      traceblk.times[traceblk.ndx_next] = traceblk.time_newest = time; // insert new entry timestamp
      for (int trk = 0; trk < ntrks; ++trk)  // and new voltages
         traceblk.voltages[traceblk.ndx_next][trk] =
#if DESKEW
            trkstate[trk].v_now;
#else
            sample->voltage[trk];
#endif
      traceblk.datacount[traceblk.ndx_next] = t->datacount; // and "extra credit" info for the special track
      traceblk.agc_gain[traceblk.ndx_next] = t->agc_gain;
      traceblk.bitspaceavg[traceblk.ndx_next] = mode ==NRZI ? nrzi.clkavg.t_bitspaceavg : t->clkavg.t_bitspaceavg;
      traceblk.t_peakdelta[traceblk.ndx_next] = t->t_peakdelta;
      for (int i = 0; tracevals[i].name != NULLP; ++i) // all other named trace values are defaulted
         for (int trk=0; trk<ntrks; ++trk)
            tracevals[i].val[traceblk.ndx_next][trk] = tracevals[i].graphbase;
      if (++traceblk.ndx_next >= TRACE_DEPTH)
         traceblk.ndx_next = 0; } };

void trace_open (void) {
   char filename[MAXPATH];
   if (!tracef) {
      sprintf(filename, "%s.trace.csv", baseoutfilename);
      assert((tracef = fopen(filename, "w")) != NULLP, "can't open trace file \"%s\"", filename);
      fprintf(tracef, "time, ,");
      for (int trk = 0; trk < ntrks; ++trk) { // titles for voltage and associated columns
         if (TRACEALL || trk == TRACETRK) {
            fprintf(tracef, "%d:volts,", trk);
            for (int i = 0; tracevals[i].name != NULLP; ++i) // any associated columns?
               if (tracevals[i].flags & T_SHOWTRK) fprintf(tracef, "%d:%s, ", trk, tracevals[i].name); // name them
         } }
      for (int i = 0; tracevals[i].name != NULLP; ++i) { // titles for event columns
         if ((tracevals[i].mode & mode)
               && !(tracevals[i].flags & T_SHOWTRK)) {
            if (!(tracevals[i].flags & T_ONLYONE)) fprintf(tracef, "T%d ", TRACETRK);
            fprintf(tracef, "%s, ", tracevals[i].name); }
         for (int j = 0; j < TRACE_DEPTH; ++j)
            for (int trk=0; trk<ntrks; ++trk)
               tracevals[i].val[j][trk] = tracevals[i].graphbase;
         tracevals[i].lastval = tracevals[i].graphbase; }
      fprintf(tracef, "  , T%d datacount, T%d AGC gain, ", TRACETRK, TRACETRK);
      if (mode == NRZI) fprintf(tracef, "bitspaceavg");
      else fprintf(tracef, "T%d bitspaceavg,", TRACETRK);
      fprintf(tracef, "T%d peak deltaT\n", TRACETRK); } }

void trace_close(void) {
   if (tracef) {
      int ndx, count;
      if (traceblk.num_entries < TRACE_DEPTH) {
         ndx = 0; count = traceblk.num_entries; }
      else {
         ndx = traceblk.ndx_next; count = TRACE_DEPTH; }
      while (count--) {
         trace_writeline(ndx);
         if (++ndx >= TRACE_DEPTH) ndx = 0; }
      fclose(tracef);
      tracef = NULLP; } };

void trace_event(enum trace_names_t tracenum, double time, float tickdirection, struct trkstate_t *t) {
   // Record an event within the list of buffered events.
   if (trace_on) {
      //rlog("adding %s=%.1f at %.8lf tick %.1f\n", tracevals[tracenum].name, tickdirection, time, TICK(time));
      assert(time <= timenow, "trace event \"%s\" at %.7lf too new at %.7lf", tracevals[tracenum].name, time, timenow);
      bool event_found = time > traceblk.time_newest - TRACE_DEPTH * traceblk.deltat;
      if (!event_found) trace_dump();
      assert(event_found, "trace event \"%s\" at %.7lf too old, at %.7lf", tracevals[tracenum].name, time, timenow);
      // find the right spot in the historical event list
      int ndx = traceblk.ndx_next - 1 - (int)((traceblk.time_newest - time) / traceblk.deltat + 0.999);
      if (ndx < 0) ndx += TRACE_DEPTH;
      if (ndx >= TRACE_DEPTH) trace_dump();
      assert(ndx < TRACE_DEPTH, "bad trace_event %s, ndx %d, time %.8lf, newest %.8lf, deltat %.2f",
             tracevals[tracenum].name, ndx, time, traceblk.time_newest, traceblk.deltat*1e6);
      if (t) // it's an event for a specific track
         tracevals[tracenum].val[ndx][t->trknum] = tracevals[tracenum].graphbase + tickdirection;
      else for (int trk=0; trk<ntrks; ++trk) // it's a global event not specific to a track
            tracevals[tracenum].val[ndx][trk] = tracevals[tracenum].graphbase + tickdirection;
      //if (tracenum == trace_clkwin)
      //   rlog("clkwin %.2f at %.7lf, tick %.1lf\n", tickdirection, time, TICK(time));//
   } };

/*****************************************************************************************************************************
   Routines used for all encoding types
******************************************************************************************************************************/

void init_blockstate(void) {	// initialize block state information for multiple reads of a block
#if 0
   skew_delaycnt[0] = 3; // 4; // TEMP TEMP TEMP for testing
   skew_delaycnt[1] = 4; // 1;
   skew_delaycnt[2] = 6; // 2;
   skew_delaycnt[3] = 13; // 9;
   skew_delaycnt[4] = 0;
   skew_delaycnt[5] = 24; // 20;
   skew_delaycnt[6] = 1; // 0;
   skew_delaycnt[7] = 19; // 16;
   skew_delaycnt[8] = 9; // 5;
#endif
   for (int parmndx=0; parmndx < MAXPARMSETS; ++parmndx) {
      assert(parmsetsptr[parmndx].active == 0 || strcmp(parmsetsptr[parmndx].id, "PRM") == 0, "bad parm block initialization");
      memset(&block.results[parmndx], 0, sizeof(struct results_t));
      block.results[parmndx].blktype = BS_NONE; } }

void init_clkavg(struct clkavg_t *c, float init_avg) { // initialize a clock averaging structure
   c->t_bitspaceavg = init_avg;
   c->bitndx = 0;
   for (int i = 0; i < CLKRATE_WINDOW; ++i) // initialize moving average bitspacing array
      c->t_bitspacing[i] = init_avg; }

void init_trackstate(void) {  // initialize all track and some block state information for a new decoding of a block
   num_trks_idle = ntrks;
   num_samples = 0;
   block.window_set = false;
#if DESKEW
   memset(&skew, 0, sizeof(skew));
#endif
   memset(&block.results[block.parmset], 0, sizeof(struct results_t));
   block.results[block.parmset].blktype = BS_NONE;
   block.results[block.parmset].alltrk_max_agc_gain = 1.0;
   memset(trkstate, 0, sizeof(trkstate));  // only need to initialize non-zeros below
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *trk = &trkstate[trknum];
      trk->trknum = trknum;
      trk->idle = true;
      trk->agc_gain = 1.0;
      trk->max_agc_gain = 1.0;
      trk->v_avg_height = PKWW_PEAKHEIGHT;
      if (!doing_density_detection) init_clkavg(&trk->clkavg, 1 / (bpi*ips));
      trk->t_clkwindow = trk->clkavg.t_bitspaceavg / 2 * PARM.clk_factor; }
   if (mode == NRZI) {
      memset(&nrzi, 0, sizeof(nrzi));
      if (!doing_density_detection) init_clkavg(&nrzi.clkavg, 1 / (bpi * ips)); } }

void show_track_datacounts (char *msg) {
   rlog("%s\n", msg);
   for (int trk=0; trk<ntrks; ++trk) {
      struct trkstate_t *t = &trkstate[trk];
      rlog("   trk %d has %d data bits, %d peaks, %f avg bit spacing\n",
           trk, t->datacount, t->peakcount, (t->t_lastbit - t->t_firstbit) / t->datacount * 1e6); } }

void check_data_alignment(int clktrk) {
   // this doesn't work for PE; track skew causes some tracks' data bits to legitimately come after other tracks' clocks!
   static int numshown = 0;
   int datacount = trkstate[0].datacount;
   for (int trknum = 0; trknum<ntrks; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      if (datacount != t->datacount && numshown<50) {
         dlog("! at clk on trk %d, trk %d has %d databytes, not %d, at %.7lf\n", clktrk, trknum, t->datacount, datacount, timenow);
         ++numshown; } } }

void adjust_agc(struct trkstate_t *t) { // update the automatic gain control level
   assert(!PARM.agc_window || !PARM.agc_alpha, "inconsistent AGC parameters in parmset %d", block.parmset);
   float gain, lastheight;
   if (PARM.agc_alpha) {  // do automatic gain control based on exponential averaging
      lastheight = t->v_lasttop - t->v_lastbot; // last peak-to-peak height
      if (lastheight > 0 /*&& lastheight < t->v_avg_height*/ ) { // if it's smaller than average  *** NO, DO THIS ALWAYS??
         gain = t->v_avg_height / lastheight;  		// the new gain, which could be less than 1
         gain = PARM.agc_alpha * gain + (1 - PARM.agc_alpha)*t->agc_gain;  // exponential smoothing with previous values
         if (gain > AGC_MAX) gain = AGC_MAX;
         if (TRACING)
            dlog("trk %d adjust gain lasttop %.2f lastbot %.2f lastheight %.2f, avgheight %.2f, old gain %.2f new gain %.2f at %.7lf tick %.1lf\n",
                 t->trknum, t->v_lasttop, t->v_lastbot, lastheight, t->v_avg_height, t->agc_gain, gain, timenow, TICK(timenow));
         t->agc_gain = gain;
         if (gain > t->max_agc_gain) t->max_agc_gain = gain; } }
   if (PARM.agc_window) {  // do automatic gain control based on the minimum of the last n peak-to-peak voltages
      assert(PARM.agc_window <= AGC_MAX_WINDOW, "AGC window too big in parmset %d", block.parmset);
      lastheight = t->v_lasttop - t->v_lastbot; // last peak-to-peak height
      if (lastheight > 0) {
         t->v_heights[t->heightndx] = lastheight; // add the new height to the window
         if (++t->heightndx >= PARM.agc_window) t->heightndx = 0;
         float minheight = 99;
         for (int i = 0; i < PARM.agc_window; ++i) if (t->v_heights[i] < minheight) minheight = t->v_heights[i];
         assert(minheight < 99, "bad minimum peak-to-peak voltage for trk %d", t->trknum);
         if (1 /*minheight < t->v_avg_height*/) { // if min peak-to-peak voltage is lower than during preamble  *** NO, DO THIS ALWAYS?
            gain = t->v_avg_height / minheight;  // what gain we should use; could be less than 1
            if (gain > AGC_MAX) gain = AGC_MAX;
            t->agc_gain = gain;
            //if (TRACING)
            //   dlog("adjust_gain: trk %d lastheight %.3fV, avgheight %.3f, minheight %.3f, heightndx %d, datacount %d, gain is now %.3f at %.7lf\n",
            //        t->trknum, lastheight, t->v_avg_height, minheight, t->heightndx, t->datacount, gain, timenow);
            if (gain > t->max_agc_gain) t->max_agc_gain = gain; } } } }

void adjust_clock(struct clkavg_t *c, float delta, int trk) {  // update the bit clock speed estimate
   int clk_window = PARM.clk_window;
   float clk_alpha = PARM.clk_alpha;
   float prevdelta = c->t_bitspaceavg;
   if (clk_window > 0) { // *** STRATEGY 1: do moving-window averaging
      float olddelta = c->t_bitspacing[c->bitndx]; // save value going out of average
      c->t_bitspacing[c->bitndx] = delta; // insert new value
      if (++c->bitndx >= clk_window) c->bitndx = 0; // circularly increment the index
      c->t_bitspaceavg += (delta - olddelta) / clk_window; // update moving average ** HAS CUMULATIVE ROUNDOFF ERRORS!
   }
   else if (clk_alpha > 0) { // *** STRATEGY 2: do exponential weighted averaging
      c->t_bitspaceavg = // exponential averaging of clock rate
         clk_alpha * delta // weighting of new value
         + (1 - clk_alpha) * c->t_bitspaceavg; // weighting of old values
   }
   else { // *** STRATEGY 3: use a constant instead of averaging
      assert(bpi > 0, "bpi=0 in adjust_clock at %.7lf", timenow);
      c->t_bitspaceavg = mode == PE ? 1 / (bpi*ips) : nrzi.clkavg.t_bitspaceavg; //
   }
   if (DEBUG && trace_on /* && trk==TRACETRK*/)
      dlog("trk %d adjust clock of %.2f with delta %.2f uS to %.2f at %.7lf tick %.1lf\n",
           trk, prevdelta*1e6, delta*1e6, c->t_bitspaceavg*1e6, timenow, TICK(timenow)); //
}
void force_clock(struct clkavg_t *c, float delta, int trk) { // force the clock speed
      for (int i = 0; i < CLKRATE_WINDOW; ++i) c->t_bitspacing[i] = delta;
      c->t_bitspaceavg = delta; }

void end_of_block(void) {
   if (mode == PE) pe_end_of_block();
   else if (mode == NRZI) nrzi_end_of_block();
   else if (mode == GCR) gcr_end_of_block(); }

void show_window(struct trkstate_t *t) {
   rlog("trk %d window at %.7lf after adding %f, left=%d, right=%d:\n",
        t->trknum, timenow, t->v_now, t->pkww_left, t->pkww_right);
   for (int ndx = t->pkww_left; ; ) {
      rlog(" %f", t->pkww_v[ndx]);
      if (t->pkww_v[ndx] == t->pkww_minv) rlog("m");
      if (t->pkww_v[ndx] == t->pkww_maxv) rlog("M");
      if (ndx == t->pkww_right) break;
      if (++ndx >= pkww_width) ndx = 0; }
   rlog("\n"); }

double process_peak (struct trkstate_t *t, float val, bool top, float required_rise) {
   // we see the shape of a peak (bottom or top) in this track's window
   if (t->idle) { // we're coming out of idle
      --num_trks_idle;
      t->idle = false;
      dlog("trk %d not idle, %d idle at %.7f tick %.1lf, AGC %.2f, v_now %f, v_lastpeak %f at %.7lf tick %.1lf, bitspaceavg %.2f\n", //
           t->trknum, num_trks_idle, timenow, TICK(timenow), t->agc_gain, t->v_now,
           t->v_lastpeak, t->t_lastpeak, TICK(t->t_lastpeak), t->clkavg.t_bitspaceavg*1e6); //
      if (FAKE_BITS && mode == PE && t->datablock && t->datacount > 1)
         //  For PE, if transitions have returned within a data block after a gap.
         //  Add extra data bits that are same as the last bit before the gap started,in an
         //  attempt to keep this track in sync with the others. .
         pe_generate_fake_bits(t); }
   int left_distance = 1;
   int ndx, nextndx, prevndx = -1;
   float time_adjustment = 0;
   for (ndx = t->pkww_left; ;) { // find where the peak is in the window
      if (t->pkww_v[ndx] == val) { // we found an instance of the peak
         assert(left_distance < pkww_width, "trk %d peak of %.3fV is at right edge, ndx=%d", t->trknum, val, ndx);
         assert(prevndx != -1, "trk %d peak of %.3fV is at left edge, ndx=%d", t->trknum, val, ndx);
         nextndx = ndx + 1; if (nextndx >= pkww_width) nextndx = 0;
         // use a 3-bit window to interpolate the best time between equal or close peaks
         if (top) {
            // there are four cases, but only two result in adjustments to the time of the peak
            float val_minus = val - PEAK_THRESHOLD / t->agc_gain;
            if (t->pkww_v[prevndx] > val_minus // if the previous value is close to the peak
                  && t->pkww_v[nextndx] < val_minus) // and the next value isn't
               time_adjustment = -0.5;  // then shift half a sample time earlier
            else if (t->pkww_v[nextndx] > val_minus // if the next value is close to the peak
                     && t->pkww_v[prevndx] < val_minus) // and the previous value isn't
               time_adjustment = +0.5;  // then shift half a sample time later
         }
         else { // bot: the same four cases, mutatis mutandis
            float val_plus = val + PEAK_THRESHOLD / t->agc_gain;
            if (t->pkww_v[prevndx] < val_plus // if the previous value is close to the peak
                  && t->pkww_v[nextndx] > val_plus) // and the next value isn't
               time_adjustment = -0.5;  // then shift half a sample time earlier
            else if (t->pkww_v[nextndx] < val_plus // if the next value is close to the peak
                     && t->pkww_v[prevndx] > val_plus) // and the previous value isn't
               time_adjustment = +0.5;  // then shift half a sample time later
         }
         double time = timenow - ((float)(pkww_width - left_distance) - time_adjustment) * sample_deltat;
         if (TRACING && (t->datablock || nrzi.datablock)) {
            //if (time_adjustment != 0)
            //   dlog("trk %d peak adjust by %4.1f, leftdst %d, rise %.3fV, AGC %.2f, left %.3fV, peak %.3fV, right %.3fV\n",
            //        t->trknum, time_adjustment, left_distance, required_rise, t->agc_gain, t->pkww_v[prevndx], val, t->pkww_v[nextndx]);
            //dlog("trk %d peak of %.3fV at %.7lf tick %.1lf found at %.7lf tick %.1lf, AGC %.2f\n",
            //     t->trknum, val, time, TICK(time), timenow, TICK(timenow), t->agc_gain);
            //show_window(t);//
         }
         t->pkww_countdown = left_distance;  // how long to be blind until the peak exits the window
         return time; }
      ++left_distance;
      if (ndx == t->pkww_right) break;
      prevndx = ndx;
      if (++ndx >= pkww_width) ndx = 0; }
   fatal( "Can't find max or min %f in trk %d window at time %.7lf", val, t->trknum, timenow);
   return 0; }

//-----------------------------------------------------------------------------
//    Process one analog voltage sample with data for all tracks.
//    Return with the updated status of the block we're working on.
//-----------------------------------------------------------------------------
enum bstate_t process_sample(struct sample_t *sample) {

   if (sample == NULLP) { // special code for endfile
      end_of_block();
      return BS_NONE; }

#if DESKEW
   for (int trknum = 0; trknum < ntrks; ++trknum) { // preprocess all tracks to do deskewing
      struct trkstate_t *t = &trkstate[trknum];
      if (skew_delaycnt[trknum] == 0) t->v_now = sample->voltage[trknum]; // no skew delay for this track
      else {
         struct skew_t *skewp = &skew[trknum];
         t->v_now = skewp->vdelayed[skewp->ndx_next]; // use the oldest voltage
         skewp->vdelayed[skewp->ndx_next] = sample->voltage[trknum]; // store the newest voltage, FIFO order
         if (++skewp->ndx_next >= skew_delaycnt[trknum]) skewp->ndx_next = 0; } }
#endif

#if TRACEFILE
   // create one entry for this sample
   trace_newtime(timenow, sample_deltat, sample, &trkstate[TRACETRK]);
   // or create some number of evenly-spaced trace entries for this sample, for more accuracy?
   //trace_newtime(timenow, sample_deltat / 2, sample, &trkstate[TRACETRK]);
   //trace_newtime(timenow + sample_deltat / 2, sample_deltat / 2, sample, &trkstate[TRACETRK]);
#endif

   if (interblock_expiration && timenow < interblock_expiration) // if we're waiting for an IBG to really start
      goto exit;

   if (nrzi.datablock && timenow > nrzi.t_lastclock + (1 + NRZI_MIDPOINT)*nrzi.clkavg.t_bitspaceavg)
      nrzi_midbit();  // do NRZI mid-bit computations

   for (int trknum = 0; trknum < ntrks; ++trknum) {  // look at the analog signal on each track
      struct trkstate_t *t = &trkstate[trknum];
#if !DESKEW
      t->v_now = sample->voltage[trknum]; // no deskewing was done in preprocessing
#endif
      if (mode == GCR && t->datablock
            && timenow > t->t_lastpeak + GCR_IDLE_THRESH * t->clkavg.t_bitspaceavg) { // if no peaks for too long
         t->datablock = false; // then we're at the end of the block for this track
         t->idle = true;
         dlog("trk %d becomes idle, %d idle at %.7f tick %.1lf, AGC %.2f, v_now %f, v_lastpeak %f, bitspaceavg %.2f\n", //
              t->trknum, num_trks_idle + 1, timenow, TICK(timenow), t->agc_gain, t->v_now, t->v_lastpeak, t->clkavg.t_bitspaceavg*1e6);
         //show_track_datacounts("at idle"); //TEMP
         if (++num_trks_idle >= ntrks) { // and maybe for all tracks
            gcr_end_of_block(); } }

      // The zero-banding we tried doesn't work because jitter near the band edge causes false peaks
      //float zero_band = PARM.zero_band / t->agc_gain;
      //if (t->v_now < zero_band && t->v_now > -zero_band) t->v_now = 0;

      //if (trace_on && mode == PE && timenow - t->t_lastbit > t->t_clkwindow)
      //   TRACE(clkwin, timenow, DNTICK, t); // stop the clock window

      if (t->t_lastpeak == 0) {  // if this is the first sample for this block
         t->pkww_v[0] = t->v_now;  // initialize moving window
         t->pkww_maxv = t->pkww_minv = t->v_now;  // (left/right is already zero)
         t->v_lastpeak = t->v_now;  // initialize last known peak
         t->t_lastpeak = timenow;
         //if (t->trknum == TRACETRK) show_window(t);
         break; }

      // incorporate this new datum as the right edge of the moving window, discard the value
      // at the left edge, and efficiently keep track of the min and max values within the window

      float old_left = 0;
      if (++t->pkww_right >= pkww_width) t->pkww_right = 0;  // make room for an entry on the right
      if (t->pkww_right == t->pkww_left) {  // if we bump into the left datum (ie, window has filled)
         old_left = t->pkww_v[t->pkww_left]; // then save the old left value
         if (++t->pkww_left >= pkww_width) t->pkww_left = 0; // and  delete it
      }
      t->pkww_v[t->pkww_right] = t->v_now;  // add the new datum on the right
      //if (TRACING && t->datablock) dlog("at tick %.1lf adding %.2fV, left %.2fV, right %.2fV, min %.2fV, max %.2fV\n",
      //         TICK(timenow), t->v_now, t->pkww_v[t->pkww_left], t->pkww_v[t->pkww_right], t->pkww_minv, t->pkww_maxv);
      if (t->v_now > t->pkww_maxv)   // it's the new max
         t->pkww_maxv = t->v_now;
      else if (t->pkww_minv < t->pkww_minv) // it's the new min
         t->pkww_minv = t->v_now;
      if (old_left == t->pkww_maxv || old_left == t->pkww_minv) {  // iff we removed the old min or max: must rescan the window values
         float maxv = -100, minv = +100;
         for (int ndx = t->pkww_left; ; ) {
            maxv = max(maxv, t->pkww_v[ndx]);
            minv = min(minv, t->pkww_v[ndx]);
            if (ndx == t->pkww_right) break;
            if (++ndx >= pkww_width) ndx = 0; }
         t->pkww_maxv = maxv;
         t->pkww_minv = minv; }
      //if (t->trknum == TRACETRK) show_window(t);

      if (t->pkww_countdown) {  // if we're waiting for a previous peak to exit the window
         --t->pkww_countdown; } // don't look a the shape within the window yet

      else { // see if the window has the profile of a new top or bottom peak
         float required_rise = PARM.pkww_rise * (t->v_avg_height / (float)PKWW_PEAKHEIGHT) / t->agc_gain;  // how much of a voltage rise constitutes a peak
         //float required_rise = PARM.pkww_rise; //TEMP
         //if (TRACING) dlog("trk %d at %.7lf tick %.1f req rise %.3f, avg height %.2f, AGC %.2f\n",
         //                     t->trknum, timenow, TICK(timenow), required_rise, t->v_avg_height, t->agc_gain);
         if (t->pkww_maxv > t->pkww_v[t->pkww_left] + required_rise
               && t->pkww_maxv > t->pkww_v[t->pkww_right] + required_rise  // the max is a lot higher than the left and right sides
               && (PARM.min_peak == 0 || t->pkww_maxv > PARM.min_peak)) {  // and it's higher than the min peak, if given
            t->v_top = t->pkww_maxv;  // which means we hit a top peak
            t->t_top = process_peak(t, t->pkww_maxv, true, required_rise);
            if (TRACING) dlog("trk %d top of %.3fV, left rise %.3f, right rise %.3f, req rise %.3f, avg ht %.3f, AGC %.2f at %.7lf tick %.1f \n",
                                 t->trknum, t->v_top,
                                 t->pkww_maxv - t->pkww_v[t->pkww_left], t->pkww_maxv - t->pkww_v[t->pkww_right], required_rise,
                                 t->v_avg_height, t->agc_gain, t->t_top, TICK(t->t_top));
            TRACE(peak, t->t_top, UPTICK, t);
            ++t->peakcount;
            if (doing_density_detection) {
               if (estden_transition((float)(t->t_top - t->t_lastpeak)))
                  block.results[block.parmset].blktype = BS_ABORTED; // got enough transitions for density detect
            }
            else {
               if (mode == PE) pe_top(t);
               else if (mode == NRZI) nrzi_top(t);
               else if (mode == GCR) gcr_top(t); }
            t->v_lasttop = t->v_top;
            t->v_lastpeak = t->v_top;
            t->t_lastpeak = t->t_top; }

         else if (t->pkww_minv < t->pkww_v[t->pkww_left] - required_rise
                  && t->pkww_minv < t->pkww_v[t->pkww_right] - required_rise  // the min is a lot lower than the left and right sides
                  && (PARM.min_peak == 0 || t->pkww_minv < -PARM.min_peak)) { // and it's lower than the min peak, if given
            t->v_bot = t->pkww_minv;  //so we hit a bottom peak
            t->t_bot = process_peak(t, t->pkww_minv, false, required_rise);
            if (TRACING) dlog("trk %d bot of %.3fV, left rise %.3f, right rise %.3f, req rise %.3f, avg ht %.3f, AGC %.2f at %.7lf tick %.1f \n",
                                 t->trknum, t->v_bot,
                                 t->pkww_v[t->pkww_left] - t->pkww_minv, t->pkww_v[t->pkww_right] - t->pkww_minv, required_rise,
                                 t->v_avg_height, t->agc_gain, t->t_bot, TICK(t->t_bot));
            TRACE(peak, t->t_bot, DNTICK, t);
            ++t->peakcount;
            if (doing_density_detection) {
               if (estden_transition((float)(t->t_bot - t->t_lastpeak)))
                  block.results[block.parmset].blktype = BS_ABORTED; // got enough transitions for density detect
            }
            else {
               if (mode == PE) pe_bot(t);
               else if (mode == NRZI) nrzi_bot(t);
               else if (mode == GCR) gcr_bot(t); }
            t->v_lastbot = t->v_bot;
            t->v_lastpeak = t->v_bot;
            t->t_lastpeak = t->t_bot; } }

      if (mode == PE && !t->idle && t->t_lastpeak != 0 && timenow - t->t_lastpeak > t->clkavg.t_bitspaceavg * PE_IDLE_FACTOR) {
         // We waited too long for a PE peak: declare that this track has become idle.
         // (NRZI track data, on the other hand, is allowed to be idle indefinitely.)
         t->v_lastpeak = t->v_now;
         TRACE(clkdet, timenow, DNTICK, t);
         dlog("trk %d became idle at %.7lf, %d idle, AGC %.2f, last peak at %.7lf, bitspaceavg %.2f usec, datacount %d\n", //
              trknum, timenow, num_trks_idle + 1, t->agc_gain, t->t_lastpeak, t->clkavg.t_bitspaceavg*1e6, t->datacount);
         t->idle = true;
         if (++num_trks_idle >= ntrks) {
            end_of_block(); } }

   } // for tracks

#if TRACEFILE
//**** Choose a test here for turning the trace on, depending on what anomaly we're looking at...
   //if (timenow > 6.2213584
         //if (timenow > 13.1369 && trkstate[0].datacount > 270
         //if (num_samples == 8500
         if (trkstate[TRACETRK].peakcount > 0
         //if (trkstate[TRACETRK].datacount >= 90
         //if (trkstate[TRACETRK].v_now < -2.0
         //if (trkstate[4].datacount > 0
         //if (trkstate[5].v_now > 0.5
         //if (nrzi.clkavg.t_bitspaceavg > 40e-6
         //if (nrzi.datablock
         //if (!trkstate[TRACETRK].idle
         //if (trace_start
         //
         && !doing_deskew && !trace_on && !trace_done) {
      trace_open();
      trace_on = true;
      torigin = timenow - sample_deltat;
      dlog("-----> trace started at %.7lf tick %.1lf\n", timenow, TICK(timenow)); }
   if (trace_on && ++trace_lines > 1000) { //**** limit on how much trace data to collect
      trace_on = false;
      trace_done = true;
      dlog("-----> trace stopped at %.7lf tick %.1lf\n", timenow, TICK(timenow));
      trace_close(); }
#endif // TRACEFILE

exit:
   ++num_samples;
   return block.results[block.parmset].blktype;  // what block type we found, if any
}

//*
