//file: decoder.c
/**********************************************************************

Decode analog magnetic tape data in one of several flavors:
  -- Manchester phase encoded (PE)
  -- NRZI (non-return-to-zero inverted) encoded
  -- GCR group coded recording (coming soon)

We are called once for each set of read head voltages on 9 data tracks.

Each track is processed independently, so that head and data skew
(especially for PE, which is self-clocking) is irrelevant. We look for
relative minima and maxima of the head voltage that represent the
downward and upward flux transitions.

For PE we use the timing of the transitions to reconstruct the original
data that created the Manchester encoding: a downward flux transition
for 0, an upward flux transition for 1, and clock transitions as needed
in the midpoint between the bits.

For NRZI we recognize either transition as a 1 bit, and the lack of
a transition at the expected bit time as a 0 bit. With vertical odd
parity for 7- and 9-track tapes, at least one track will have a
transition at each bit time.

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

If we see a total dropout in a track, we wait for data to
return. Then we create "faked" bits to cover for the dropout, and
keep a record of which bits have been faked. Unfortunately this
doesn't work as well for NRZI, since the dropout might be in the
only track that has transitions and we have to estimate the clock
by dead-reckoning.

The major routine here is process_sample(). It returns an indication
of whether the current sample represents the end of a block, whose
state is then in struct blkstate_t block.

----> See readtape.c for the merged change log.

***********************************************************************
Copyright (C) 2018, Len Shustek
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

#include "decoder.h"

// Variables for time relative to the start must be double-precision.
// Delta times for samples close to each other can be single-precision,
// although the compiler will complain about loss of precision.

double timenow=0;
double torigin = 0;
int num_trks_idle;
int num_samples;
int errcode=0;
double interblock_expiration = 0; // interblock gap expiration time

struct nrzi_t nrzi = { 0 }; // NRZI decoding status
float sample_deltat = 0;
int pkww_width = 0;

struct trkstate_t trkstate[MAXTRKS] = { // the current state of all tracks
   0 };
uint16_t data[MAXBLOCK+1] = { 		  // the reconstructed data in bits 8..0 for tracks 0..7, then P as the LSB
   0 };
uint16_t data_faked[MAXBLOCK+1] = {   // flag for "data was faked" in bits 8..0 for tracks 0..7, then P as the LSB
   0 };
double data_time[MAXBLOCK+1] = { 	  // the time the last track contributed to this data byte
   0 };
struct blkstate_t block;  // the status of the current data block as decoded with the various sets of parameters

#if PEAK_STATS
#define PEAK_STATS_NUMBUCKETS 100
#define PEAK_STATS_BINWIDTH 0.5e-6
double peak_stats_leftbin = 0;
int peak_counts[MAXTRKS][PEAK_STATS_NUMBUCKETS] = { 0 };
int peak_trksums[MAXTRKS] = { 0 };
#endif

/***********************************************************************************************************************
Routines for accumulating flux transition times, and using that
to deskew the input data by delaying some of the channel data
************************************************************************************************************************/

#if DESKEW
struct skew_t {
   float vdelayed[MAXSKEWSAMP]; // the buffered voltages
   int ndx_next;            // the next slot to use for the newest data
} skew[MAXTRKS];            // (This structure is cleared at the start of each block.)
int skew_delaycnt[MAXTRKS] = { 0 };    // the skew delay, in number of samples, for each track. (This is persistent.)

void skew_set_delay(int trknum, float time) { // set the skew delay for a track
   assert(sample_deltat, "delta T not set yet in skew_set_delay");
   assert(time >= 0, "negative skew amount %f", time);
   int delay = (time + sample_deltat/2) / sample_deltat;
   //rlog("set skew trk %d to %f usec, %d samples\n", trknum, time*1e6, delay);
   if (delay > MAXSKEWSAMP) rlog("track %d skew of %.1f usec is too big\n", trknum, time*1e6);
   skew_delaycnt[trknum] = min(delay, MAXSKEWSAMP); };

void skew_set_deskew(void) {  // set all the deskew amounts based on where we see most of the transitions
   int trk, bkt;
   long long int avgsum;
   float avg[MAXTRKS]; // the average peak position for each track
   for (trk = 0; trk < ntrks; ++trk) {
      for (avgsum = bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt) {
         avgsum += peak_counts[trk][bkt] * (PEAK_STATS_BINWIDTH*1e6 * bkt + peak_stats_leftbin * 1e6); }
      avg[trk] = (float)avgsum / (float)peak_trksums[trk]; }
   float maxavg = 0;
   for (trk = 0; trk < ntrks; ++trk) // see which track has the last transitions, on average
      maxavg = max(maxavg, avg[trk]);
   for (trk = 0; trk < ntrks; ++trk) {
      //rlog("trk %d has %d transitions, avg %f max %f\n", trk, peak_trksums[trk], avg[trk], maxavg);
      skew_set_delay(trk, peak_trksums[trk] > 0 ? (maxavg - avg[trk]) / 1e6 : 0); // delay the other tracks relative to it
   }
   if (!quiet) skew_display(); }

int skew_min_transitions(void) {
   int min_transitions = INT_MAX;
   for (int trknum = 0; trknum < ntrks; ++trknum)
      if (peak_trksums[trknum] < min_transitions) min_transitions = peak_trksums[trknum];
   return min_transitions; }

void skew_display(void) { // show the track skews
   for (int trknum = 0; trknum < ntrks; ++trknum)
      rlog("  track %d delayed by %d clocks (%.2f usec) based on %d observed flux transitions\n",
           trknum, skew_delaycnt[trknum], skew_delaycnt[trknum] * sample_deltat * 1e6, peak_trksums[trknum]); };
#endif

#if PEAK_STATS
void output_peakstats(void) { // Create an Excel .CSV file with flux transition position statistics
   FILE *statsf;
   char filename[MAXPATH];
   int trk, bkt;
   int totalcount = 0;
   long long int avgsum;
   sprintf(filename, "%s\\stats.csv", basefilename);
   assert(statsf = fopen(filename, "w"), "can't open stats file \"%s\"", filename);
   for (bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt)
      fprintf(statsf, ",%.1f uS", PEAK_STATS_BINWIDTH*1e6 * bkt + peak_stats_leftbin * 1e6);
   fprintf(statsf, ",avg uS\n");
   for (trk = 0; trk < ntrks; ++trk) {
      for (avgsum = bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt) {
         avgsum += peak_counts[trk][bkt] * (PEAK_STATS_BINWIDTH*1e6 * bkt + peak_stats_leftbin * 1e6); }
      fprintf(statsf, "trk%d,", trk);
      for (bkt = 0; bkt < PEAK_STATS_NUMBUCKETS; ++bkt)
         fprintf(statsf, "%.2f%%, ", 100 * (float)peak_counts[trk][bkt] / (float)peak_trksums[trk]);
      fprintf(statsf, "%.2f\n", (float)avgsum / (float)peak_trksums[trk]);
      totalcount += peak_trksums[trk]; }
   fclose(statsf);
   if (!quiet) rlog("created statistics file \"%s\" from %s measurements of flux transition positions\n", filename, intcommas(totalcount)); }

#endif

/***********************************************************************************************************************
Routines for estimating the bit density on the tape, based on
the first thousand samples of flux transition timing
************************************************************************************************************************/

#define ESTDEN_BINWIDTH 0.5e-6      // quantize of transition delta times into bins of this width, in seconds
#define ESTDEN_MAXDELTA 120e-6      // ignore transition delta times bigger than this
#define ESTDEN_NUMBINS 100          // how many bins for different delta times we have
#define ESTDEN_COUNTNEEDED 1000     // how many transitions we need to see for a good estimate
#define ESTDEN_MINPERCENT 5         // the minimum transition delta time must be seen at least this many percent of the total
#define ESTDEN_CLOSEPERCENT 20      // how close, in percent, to one of the standard densities we need to be

struct {
   int deltas[ESTDEN_NUMBINS];   // distance between transitions, in seconds/ESTDEN_BINWIDTH
   int counts[ESTDEN_NUMBINS];   // how often we've seen that distance
   int binsused;
   int totalcount; } estden;

void estden_init(void) {
   memset(&estden, 0, sizeof(estden)); }

void estden_transition(float deltasecs) { // count a transition distance
   int delta = deltasecs / ESTDEN_BINWIDTH;  // round down to multiple of BINWIDTH
   int ndx;
   assert(deltasecs > 0, "negative delta %f in estden_transition", deltasecs);
   if (deltasecs <= ESTDEN_MAXDELTA) {
      for (ndx = 0; ndx < estden.binsused; ++ndx) // do we have it already?
         if (estden.deltas[ndx] == delta) break;  // yes
      if (ndx >= estden.binsused) { // otherwise create a new bucket
         assert(estden.binsused < ESTDEN_NUMBINS, "estden: too many transition delta values: %d", estden.binsused);
         estden.deltas[estden.binsused++] = delta; }
      ++estden.counts[ndx];
      ++estden.totalcount; } }

bool estden_done(void) {
   return estden.totalcount >= ESTDEN_COUNTNEEDED; }

void estden_show(void) {
   rlog("density estimation buckets, %.2f usec each:\n", ESTDEN_BINWIDTH*1e6);
   for (int ndx = 0; ndx < estden.binsused; ++ndx)
      rlog(" %2d: %5.1f usec cnt %d\n", ndx, estden.deltas[ndx] * ESTDEN_BINWIDTH * 1e6, estden.counts[ndx]); }

void estden_setdensity(int numblks) { // figure out the tape density
   static int standard_densities[] = { 200, 556, 800, 1600, 9214 /* GCR at "6250" */, 0 };
   int ndx;
   int mindist = INT_MAX;
   // Look for the smallest transition distance that occurred at least 5% of the time.
   // That lets us ignore a few noise glitches and bizarre cases, should they occur.
   for (ndx = 0; ndx < estden.binsused; ++ndx) {
      if (estden.counts[ndx] > estden.totalcount * ESTDEN_MINPERCENT / 100
            && estden.deltas[ndx] < mindist)
         mindist = estden.deltas[ndx]; }
   // estimate the density from that minimum transition delta time
   float density = 1.0 / (ips * (float)(mindist+0.5) * ESTDEN_BINWIDTH);
   if (mode == PE) density /= 2;  // twice the transitions for PE
   // search for a standard density that's close
   for (int ndx = 0; standard_densities[ndx]; ++ndx) {
      float stddensity = standard_densities[ndx];
      if (abs(density - stddensity) < stddensity * ESTDEN_CLOSEPERCENT / 100) {
         bpi = stddensity;
         if (!quiet) rlog("Set density to %.0f BPI after reading %d blocks and seeing %d transitions implying %.0f BPI\n",
              bpi, numblks, estden.totalcount, density);
         return; } }
   fatal("The detected density of %.0f (%.1f usec) after seeing %d transitions is non-standard; please specify", 
      density, (float)(mindist + 0.5) * ESTDEN_BINWIDTH * 1e6, estden.totalcount); }

/*****************************************************************************************************************************
Routines for creating the trace file
******************************************************************************************************************************/
/* This stuff creates a CSV trace file with one or all tracks of voltage data
plus all sorts of debugging event info. To see the timeline, create a
line graph in Excel from column C through the next blank column.

The compiler switch that turns this on, and the track number to record special
info about, is at the top of decoder.h.
The start and end of the graph is controlled by code at the bottom of this file.

The trace data is buffered before being written to the file so that we can
"rewrite history" for events that are discovered late. That happens, for
example, because the new moving-window algorithm for peak detection finds
the peaks several clock ticks after they actually happen.
*/

bool trace_on = false, trace_done = false;
int trace_lines = 0;
FILE *tracef;

#define TRACE_DEPTH 60
#define TRACE(var,time,tickdirection,t) {if(TRACEFILE) trace_event(trace_##var, time, tickdirection, t);}
#define TRACING (DEBUG && trace_on && t->trknum == TRACETRK)

#define TB 1.00   // base display level for miscellaneous events
#define TS 3.00   // track separation, below zero
#define UPTICK 0.25
#define DNTICK -0.25

struct trace_val_t {  // the trace history buffer for events
   char *name;          // what it's called
   enum mode_t mode;    // which encoding mode this is for (or ALL)
   int flags;           // any combination of T_xxxx flags
#define T_PERSISTENT 0x01  // show the value indicated by the last up or down transition
#define T_SHOWTRK 0x02     // show this with the track at the level of the voltage
#define T_ONLYONE 0x04     // there is only one of these, not one per track
   float graphbase;     // the baseline output graph y-axis position
   float lastval;       // the last y-axis position output (for persistent values)
   float val[TRACE_DEPTH][MAXTRKS]; // all the values for all the tracks
}
tracevals[] = {
   { "peak",   ALL,  T_SHOWTRK,     0 },
   { "data",   ALL,  T_PERSISTENT, TB + 0.0 },
   { "avgpos", NRZI, T_ONLYONE,    TB + 1.0 },
   { "midbit", NRZI, T_ONLYONE,    TB + 1.5 },
   { "clkedg", PE,   0,            TB + 2.0 },
   { "datedg", PE,   0,            TB + 2.5 },
   { "clkwin", PE,   T_PERSISTENT, TB + 3.0 },
   { "clkdet", PE,   T_PERSISTENT, TB + 3.5 },
   { NULLP } };
enum trace_names_t { // must match the list above!
   trace_peak, trace_data, trace_avgpos, trace_midbit, trace_clkedg, trace_datedg, trace_clkwin, trace_clkdet };

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
   float bitspaceavg[TRACE_DEPTH];//
}
traceblk = {0 };

void trace_dump(void) { // display the entire trace buffer
   rlog("trace buffer after %d entries\n", traceblk.num_entries);
   int ndx = traceblk.ndx_next;
   for (int i = 0; i < TRACE_DEPTH; ++i) {
      if (traceblk.times[ndx] != 0) {
         rlog("%2d: %.8lf, %5.2lf, ", ndx, traceblk.times[ndx], (traceblk.time_newest - traceblk.times[ndx])*1e6);
         for (int i = 0; tracevals[i].name != NULLP; ++i)
            rlog("%s:%5.2f, ", tracevals[i].name, tracevals[i].val[ndx]);
         rlog("\n"); }
      if (++ndx >= TRACE_DEPTH) ndx = 0; } };

void trace_writeline(int ndx) {  // write out one buffered trace line
   if (tracef) {
      fprintf(tracef, "%.8lf, ,", traceblk.times[ndx]);
      for (int trk = 0; trk < ntrks; ++trk) { // for all the tracks we're doing
         if (TRACEALL || trk == TRACETRK) {
            int level = -(trk + 1) * TS; // base y-axis level for this track
            fprintf(tracef, "%.4f,",  // output voltage
                    traceblk.voltages[ndx][trk] * TRACESCALE + level);
            for (int i = 0; tracevals[i].name != NULLP; ++i) // do other associated columns
               if (tracevals[i].flags & T_SHOWTRK)
                  fprintf(tracef, "%.2f, ", tracevals[i].val[ndx][trk] + level); } }
      for (int i = 0; tracevals[i].name != NULLP; ++i) { // do the events not shown with the tracks
         if (tracevals[i].mode == ALL || (tracevals[i].mode == mode)) {
            if (!(tracevals[i].flags & T_PERSISTENT) ||
                  tracevals[i].val[ndx][TRACETRK] != tracevals[i].graphbase)
               tracevals[i].lastval = tracevals[i].val[ndx][TRACETRK];
            if ( !(tracevals[i].flags & T_SHOWTRK)) fprintf(tracef, "%f, ", tracevals[i].lastval); } }
      fprintf(tracef, ", %d, %.2f, %.2f\n", // do the extra-credit stuff
              traceblk.datacount[ndx], traceblk.agc_gain[ndx], traceblk.bitspaceavg[ndx]*1e6); } }

void trace_newtime(double time, float deltat, struct sample_t *sample, struct trkstate_t *t) {
   // create a new timestamped entry in the trace history buffer
   if (tracef && trace_on) {
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
      traceblk.bitspaceavg[traceblk.ndx_next] = mode == NRZI ? nrzi.clkavg.t_bitspaceavg : t->clkavg.t_bitspaceavg;
      for (int i = 0; tracevals[i].name != NULLP; ++i) // all other named trace values are defaulted
         for (int trk=0; trk<ntrks; ++trk)
            tracevals[i].val[traceblk.ndx_next][trk] = tracevals[i].graphbase;
      if (++traceblk.ndx_next >= TRACE_DEPTH)
         traceblk.ndx_next = 0; } };

void trace_open (void) {
   char filename[MAXPATH];
   if (!tracef) {
      sprintf(filename, "%s\\trace.csv", basefilename);
      assert(tracef = fopen(filename, "w"), "can't open trace file \"%s\"", filename);
      fprintf(tracef, "time, ,");
      for (int trk = 0; trk < ntrks; ++trk) { // titles for voltage and associated columns
         if (TRACEALL || trk == TRACETRK) {
            fprintf(tracef, "V%d,", trk);
            for (int i = 0; tracevals[i].name != NULLP; ++i) // any associated columns?
               if (tracevals[i].flags & T_SHOWTRK) fprintf(tracef, ", "); // leave a space for it
         } }
      for (int i = 0; tracevals[i].name != NULLP; ++i) { // titles for event columns
         if ((tracevals[i].mode == ALL || tracevals[i].mode == mode)
               && !(tracevals[i].flags & T_SHOWTRK)) {
            if (!(tracevals[i].flags & T_ONLYONE)) fprintf(tracef, "T%d ", TRACETRK);
            fprintf(tracef, "%s, ", tracevals[i].name); }
         for (int j = 0; j < TRACE_DEPTH; ++j)
            for (int trk=0; trk<ntrks; ++trk)
               tracevals[i].val[j][trk] = tracevals[i].graphbase;
         tracevals[i].lastval = tracevals[i].graphbase; }
      fprintf(tracef, "  , T%d datacount, T%d AGC gain, ", TRACETRK, TRACETRK);
      if (mode == NRZI) fprintf(tracef, "bitspaceavg\n");
      else fprintf(tracef, "T%d bitspaceavg\n", TRACETRK); } }

void trace_close(void) {
   if (tracef) {
      int numbuffered = min(traceblk.num_entries, TRACE_DEPTH);
      for (int ndx = traceblk.ndx_next; numbuffered; --numbuffered) {
         trace_writeline(ndx);
         if (++ndx >= TRACE_DEPTH) ndx = 0; }
      fclose(tracef);
      tracef = NULLP; } };

void trace_event(enum trace_names_t tracenum, double time, float tickdirection, struct trkstate_t *t) {
   if (trace_on) {
      //rlog("adding %s=%.1f at %.8lf\n", tracevals[tracenum].name, tickdirection, time);
      assert(time <= timenow, "trace event \"%s\" at %.7lf too new at %.7lf", tracevals[tracenum].name, time, timenow);
      bool event_found = time > traceblk.time_newest - TRACE_DEPTH * traceblk.deltat;
      if (!event_found) trace_dump();
      assert(event_found, "trace event \"%s\" too old at %.7lf", tracevals[tracenum].name, time);
      // find the right spot in the historical event list
      int ndx = traceblk.ndx_next - 1 - (int)((traceblk.time_newest - time) / traceblk.deltat + 0.999);
      if (ndx < 0) ndx += TRACE_DEPTH;
      if (ndx >= TRACE_DEPTH) trace_dump();
      assert(ndx < TRACE_DEPTH, "bad trace_event %s, ndx %d, time %.8lf, newest %.8lf, deltat %.2f",
             tracevals[tracenum].name, ndx, time, traceblk.time_newest, traceblk.deltat*1e6);
      if (t) // just for one track
         tracevals[tracenum].val[ndx][t->trknum] = tracevals[tracenum].graphbase + tickdirection;
      else for (int trk=0; trk<ntrks; ++trk) // a global event for all tracks
            tracevals[tracenum].val[ndx][trk] = tracevals[tracenum].graphbase + tickdirection;
      //if (tracenum == trace_clkwin)
      //   rlog("clkwin %.2f at %.7lf, tick %.1lf\n", tickdirection, time, TICK(time));//
   } };

/*****************************************************************************************************************************
   Routines for all encoding types
******************************************************************************************************************************/

void init_blockstate(void) {	// initialize block state information for multiple reads of a block
   static bool wrote_config = false;
   for (int i=0; i<MAXPARMSETS; ++i) {
      assert(parmsetsptr[i].active == 0 || strcmp(parmsetsptr[i].id, "PRM") == 0, "bad parm block initialization");
      memset(&block.results[i], 0, sizeof(struct results_t));
      block.results[i].blktype = BS_NONE; } }

void init_trackstate(void) {  // initialize all track and some block state information for a new decoding of a block
   num_trks_idle = ntrks;
   num_samples = 0;
   block.window_set = false;
   block.last_sample_time = 0;
#if DESKEW
   memset(&skew, 0, sizeof(skew));
#endif
   if (!trace_on) torigin = timenow - sample_deltat; // for cleaner error messages after the first block
   memset(&block.results[block.parmset], 0, sizeof(struct results_t));
   block.results[block.parmset].blktype = BS_NONE;
   block.results[block.parmset].alltrk_max_agc_gain = 1.0;
   memset(trkstate, 0, sizeof(trkstate));  // only need to initialize non-zeros below
   for (int trknum=0; trknum<ntrks; ++trknum) {
      struct trkstate_t *trk = &trkstate[trknum];
      trk->trknum = trknum;
      trk->idle = true;
      trk->agc_gain = 1.0;
      trk->max_agc_gain = 1.0;
      trk->v_avg_height = PKWW_PEAKHEIGHT;
      if (!doing_density_detection) {
         trk->clkavg.t_bitspaceavg = 1/(bpi*ips);
         for (int i = 0; i < CLKRATE_WINDOW; ++i) // initialize moving average bitspacing array
            trk->clkavg.t_bitspacing[i] = 1 / (bpi*ips); }
      trk->t_clkwindow = trk->clkavg.t_bitspaceavg/2 * PARM.clk_factor; }
   memset(&nrzi, 0, sizeof(nrzi));  // only need to initialize non-zeros below
   if (!doing_density_detection) nrzi.clkavg.t_bitspaceavg = 1 / (bpi * ips); }

void show_track_datacounts (char *msg) {
   dlog("%s\n", msg);
   for (int trk=0; trk<ntrks; ++trk) {
      struct trkstate_t *t = &trkstate[trk];
      dlog("   trk %d has %d data bits, %d peaks, %f avg bit spacing\n",
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
            dlog("adjust gain: trk %d lasttop %.2f lastbot %.2f lastheight %.2f, avgheight %.2f, old gain %.2f new gain %.2f at %.7lf tick %.1lf\n",
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
   if (clk_window > 0) { // *** STRATEGY 1: do moving-window averaging
      float olddelta = c->t_bitspacing[c->bitndx]; // save value going out of average
      c->t_bitspacing[c->bitndx] = delta; // insert new value
      if (++c->bitndx >= clk_window) c->bitndx = 0; // circularly increment the index
      c->t_bitspaceavg += (delta - olddelta) / clk_window; // update moving average
   }
   else if (clk_alpha > 0) { // *** STRATEGY 2: do exponential weighted averaging
      c->t_bitspaceavg = // exponential averaging of clock rate
         clk_alpha * delta // weighting of new value
         + (1 - clk_alpha) * c->t_bitspaceavg; // weighting of old values
   }
   else { // *** STRATEGY 3: use a constant instead of averaging
      assert(bpi > 0, "bpi=0 in adjust_clock at %.7lf", timenow);
      c->t_bitspaceavg = mode == PE ? 1 / (bpi*ips) : nrzi.clkavg.t_bitspaceavg; }
   if (DEBUG && trace_on && trk==TRACETRK) dlog("adjust clock with delta %.2f uS to %.2f at %.1lf\n", delta*1e6, c->t_bitspaceavg*1e6, TICK(timenow)); //
}

/*****************************************************************************************************************************
Routines for 1600 BPI Phase Encoding (PE)
******************************************************************************************************************************/

void pe_end_of_block(void) { // All/most tracks have just become idle. See if we accumulated a data block, a tape mark, or junk
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding

   // a tape mark is bizarre:
   //  -- 80 or more flux reversals (but no data bits) on tracks 0, 2, 5, 6, 7, and P
   //  -- no flux reversals (DC erased) on tracks 1, 3, and 4
   // We actually allow a couple of data bits because of weirdness when the flux transitions stop
   if (  trkstate[0].datacount <= 2 && trkstate[0].peakcount > 75 &&
         trkstate[2].datacount <= 2 && trkstate[2].peakcount > 75 &&
         trkstate[5].datacount <= 2 && trkstate[5].peakcount > 75 &&
         trkstate[6].datacount <= 2 && trkstate[6].peakcount > 75 &&
         trkstate[7].datacount <= 2 && trkstate[7].peakcount > 75 &&
         trkstate[8].datacount <= 2 && trkstate[8].peakcount > 75 &&
         trkstate[1].peakcount <= 2 &&
         trkstate[3].peakcount <= 2 &&
         trkstate[4].peakcount <= 2
      ) { // got a tape mark
      result->blktype = BS_TAPEMARK;
      return; }

   // to extract a valid data block, we remove the postammble and check that all tracks have the same number of bits
   float avg_bit_spacing = 0;
   result->minbits=MAXBLOCK;
   result->maxbits=0;
   //for (int i=60; i<120; ++i)
   //    rlog("%3d: %02X, %c, %d\n", i, data[i]>>1, EBCDIC[data[i]>>1], data[i]&1);

   for (int trk=0; trk<ntrks; ++trk) { // process postable bits on all tracks
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (t->t_lastbit - t->t_firstbit) / t->datacount;
      //dlog("trk %d firstbit at %.7lf, lastbit at %.7lf, avg spacing %.2f\n", trk, t->t_firstbit, t->t_lastbit, avg_bit_spacing*1e6);
      int postamble_bits;
      if (t->datacount > 0) {
         for (postamble_bits=0; postamble_bits<=MAX_POSTAMBLE_BITS; ++postamble_bits) {
            --t->datacount; // remove one bit
            if ((data_faked[t->datacount] & (1 << (ntrks-trk))) != 0) { // if the bit we removed was faked,
               assert(block.results[block.parmset].faked_bits>0, "bad fake data count on trk %d at %.7lf", trk, timenow);
               --block.results[block.parmset].faked_bits;  // then decrement the count of faked bits
               dlog("   remove fake bit %d on track %d\n", t->datacount, trk); //
            }
            // weird stuff goes on as the signal dies at the end of a block, so we ignore the last few data bits.
            if (postamble_bits > IGNORE_POSTAMBLE &&  	// if we've ignored the last few postamble bits
                  (data[t->datacount] & (1 << (ntrks - 1 - trk))) != 0)	// and we just passed a "1"
               break;  								// then we've erased the postable and are done
         }
         if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain;
         dlog("trk %d had %d postamble bits, max AGC %5.2f, datacount %d\n", trk, postamble_bits, t->max_agc_gain, t->datacount); }
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount; }
   result->avg_bit_spacing = avg_bit_spacing/ntrks;

   if (result->maxbits == 0) {  // leave result-blktype == BS_NONE
      dlog("   ignoring noise block at %.7lf\n", timenow); }
   else {
      if (result->minbits != result->maxbits) {  // different number of bits in different tracks
         show_track_datacounts("*** malformed block");
         result->blktype = BS_MALFORMED; }
      else {
         result->blktype = BS_BLOCK; }
      result->vparity_errs = 0;
      for (int i=0; i<result->minbits; ++i) // count parity errors
         if (parity(data[i]) != expected_parity) ++result->vparity_errs;
      result->errcount = result->vparity_errs; } }

void pe_addbit (struct trkstate_t *t, byte bit, bool faked, double t_bit) { // we encountered a data bit transition
   TRACE(data, t_bit, bit ? UPTICK : DNTICK, t);
   //if (faked) TRACE(fakedata,yes);
   TRACE(datedg, t_bit, UPTICK, t); 	// data edge
   if (t->t_lastbit != 0) TRACE(clkwin, t->t_lastbit + t->t_clkwindow, DNTICK, t); // show where the previous clock window ended
   TRACE(clkwin,t_bit, UPTICK, t);	   // start a new clock window
   if (t->t_lastbit == 0) t->t_lastbit = t_bit - 1/(bpi*ips); // start of preamble  FIX? TEMP?
   if (t->datablock) { // collecting data
      if (TRACING) dlog("trk %d add %d to %3d bytes at %.7lf, V=%.5f, AGC=%.2f\n", t->trknum, bit, t->datacount, t_bit, t->v_now, t->agc_gain);
      t->lastdatabit = bit;
      if (!t->idle && !faked) { // adjust average clock rate based on inter-bit timing
         float delta = t_bit - t->t_lastbit;
         adjust_clock(&t->clkavg, delta, t->trknum);
         t->t_clkwindow = t->clkavg.t_bitspaceavg / 2 * PARM.clk_factor; }
      t->t_lastbit = t_bit;
      if (t->datacount == 0) t->t_firstbit = t_bit; // record time of first bit in the datablock
      uint16_t mask = 1 << (ntrks - 1 - t->trknum);  // update this track's bit in the data array
      data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
      data_faked[t->datacount] = faked ? data_faked[t->datacount] | mask : data_faked[t->datacount] & ~mask;
      if (faked) ++block.results[block.parmset].faked_bits;
      data_time[t->datacount] = t_bit;
      if (t->datacount < MAXBLOCK) ++t->datacount; } }

void pe_top (struct trkstate_t *t) {  // local maximum: end of a positive flux transition
   if (t->datablock) { // inside a data block
      bool missed_transition = (t->t_top + t->t_pulse_adj) - t->t_lastpeak > t->t_clkwindow; // missed a half-bit transition?
      if (!t->clknext // if we're expecting a data transition
            || missed_transition) { // or we missed a clock transition
         if (TRACING) dlog("add 1 top at %.7lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.7lf tick %.1lf, clkwin %.2f uS\n",
                              t->t_top, TICK(t->t_top), t->t_pulse_adj*1e6, t->v_top, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         pe_addbit (t, 1, false, t->t_top);  // then we have new data '1'
         t->clknext = true; }
      else { // this was a clock transition
         TRACE(clkedg, t->t_top, UPTICK, t);
         TRACE(clkdet, t->t_top, UPTICK, t);
         if (TRACING) dlog("clk   top at %.7lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.7lf tick %.1lf, clkwin %.2f uS\n",
                              t->t_top, TICK(t->t_top), t->t_pulse_adj*1e6, t->v_top, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         t->clknext = false; }
      t->t_pulse_adj = ((t->t_top - t->t_lastpeak) - t->clkavg.t_bitspaceavg / (missed_transition ? 1 : 2)) * PARM.pulse_adj;
      adjust_agc(t); }
   else { // !datablock: inside the preamble
      if (t->peakcount > MIN_PREAMBLE	// if we've seen at least 35 zeroes
            && t->t_top - t->t_lastpeak > t->t_clkwindow) { // and we missed a clock
         t->datablock = true;	// then this 1 means data is starting (end of preamble)
         t->v_avg_height = t->v_avg_height_sum / t->v_avg_height_count; // compute avg peak-to-peak voltage
         //dlog("trk %d avg peak-to-peak is %.2fV at %.7lf\n", t->trknum, t->v_avg_height, timenow);
         assert(t->v_avg_height>0, "avg peak-to-peak voltage isn't positive");
         dlog("trk %d start data at %.7lf tick %.1lf, AGC %.2f, clk window %lf usec, avg peak-to-peak %.2fV\n", //
              t->trknum, timenow, TICK(timenow), t->agc_gain, t->t_clkwindow*1e6, t->v_avg_height); //
      }
      else { // stay in the preamble
         t->clknext = false; // this was a clock; data is next
         if (t->peakcount>=AGC_STARTBASE && t->peakcount<=AGC_ENDBASE) { // accumulate peak-to-peak voltages
            t->v_avg_height_sum += t->v_top - t->v_bot;
            ++t->v_avg_height_count;
            t->v_heights[t->heightndx] = t->v_top - t->v_bot;
            if (++t->heightndx >= PARM.agc_window) t->heightndx = 0; } } } }

void pe_bot (struct trkstate_t *t) { // local minimum: end of a negative flux transition
   if (t->datablock) { // inside a data block or the postamble
      bool missed_transition = (t->t_bot + t->t_pulse_adj) - t->t_lastpeak > t->t_clkwindow; // missed a half-bit transition?
      if (!t->clknext // if we're expecting a data transition
            || missed_transition) { // or we missed a clock transition
         if (TRACING) dlog("add 0 bot at %.7lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.7lf tick %.1lf, clkwin %.2f uS\n",
                              t->t_bot, TICK(t->t_bot), t->t_pulse_adj*1e6, t->v_bot, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         pe_addbit (t, 0, false, t->t_bot);  // then we have new data '0'
         t->clknext = true; }
      else { // this was a clock transition
         TRACE(clkedg,t->t_bot, UPTICK, t);
         TRACE(clkdet,t->t_bot, UPTICK, t);
         if (TRACING) dlog("clk   bot at %.7lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.7lf tick %.1lf, clkwin %.2f uS\n",
                              t->t_bot, TICK(t->t_bot), t->t_pulse_adj*1e6, t->v_bot, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         t->clknext = false; }
      t->t_pulse_adj = ((t->t_bot - t->t_lastpeak) - t->clkavg.t_bitspaceavg / (missed_transition ? 1 : 2)) * PARM.pulse_adj;
      adjust_agc(t); }
   else { // inside the preamble
      t->clknext = true; // force this to be treated as a data transition; clock is next
   } }

/*****************************************************************************************************************************
Routines for NRZI encoding
******************************************************************************************************************************/

void nrzi_end_of_block(void) {
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   float avg_bit_spacing = 0;
   nrzi.datablock = false;
   result->minbits = MAXBLOCK;
   result->maxbits = 0;
   for (int trk = 0; trk < ntrks; ++trk) { //
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (t->t_lastbit - t->t_firstbit) / t->datacount;
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount;
      if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain; }
   result->avg_bit_spacing = avg_bit_spacing / ntrks;
   dlog("end_of_block, min %d max %d, avgbitspacing %f, at %.7lf\n", result->minbits, result->maxbits, result->avg_bit_spacing*1e6, timenow);
   if (result->maxbits <= 1) {  // leave result-blktype == BS_NONE
      dlog("   ignoring noise block of length %d at %.7lf\n", result->maxbits, timenow); }
   else {
      if (result->minbits != result->maxbits) {  // different number of bits in different tracks
         show_track_datacounts("*** malformed block");
         result->blktype = BS_MALFORMED; }
      else { // well-formed block
         //dumpdata(data, result->minbits);
         if ((result->minbits == 3  && ntrks == 9 && data[0] == 0x26 && data[1] == 0 && data[2] == 0x26)  // 9 trk: trks 367, then nothing, then 367
               || (result->minbits == 2 && ntrks == 7 && data[0] == 0x1e && data[1] == 0x1e) // 7trk: trks 8421, then nothing, then 367
            ) result->blktype = BS_TAPEMARK; // then it's the bizarre tapemark
         else result->blktype = BS_BLOCK; }
      result->vparity_errs = 0;
      int crc = 0;
      int lrc = 0;
      if (result->blktype != BS_TAPEMARK && result->minbits > 2) {
         for (int i = 0; i < result->minbits - (ntrks==7 ? 1 : 2); ++i) {  // count parity errors, and check the CRC/LRC at the end
            if (parity(data[i]) != expected_parity) ++result->vparity_errs;
            lrc ^= data[i];
            crc ^= data[i]; // C0..C7,P  (See IBM Form A22-6862-4)
            if (crc & 2) crc ^= 0xf0; // if P will become 1 after rotate, invert what will go into C2..C5
            int lsb = crc & 1; // rotate all 9 bits
            crc >>= 1;
            if (lsb) crc |= 0x100; }
         crc ^= 0x1af; // invert all except C2 and C4; note that the CRC could be zero if the number of data bytes is odd
         if (ntrks == 9) lrc ^= crc;  // LRC inlcudes the CRC (the manual doesn't say that!)
         result->crc = data[result->minbits - 2];
         result->lrc = data[result->minbits - 1];
         result->errcount = result->vparity_errs;
         if (ntrks == 9) { // only 9-track tapes have CRC
            if (crc != result->crc) {
               result->crc_bad = true;
               ++result->errcount; }
            dlog("crc is %03X, should be %03X\n", result->crc, crc); }
         if (lrc != result->lrc) {
            result->lrc_bad = true;
            ++result->errcount; }
         dlog("lrc is %03X, should be %03X\n", result->lrc, lrc);
         result->minbits -= ntrks == 9 ? 2 : 1;  // don't include CRC (if present) and LRC in the data
         result->maxbits -= ntrks == 9 ? 2 : 1; }
      interblock_expiration = timenow + NRZI_IBG_SECS;  // ignore data for a while until we're well into the IBG
   } }

void nrzi_addbit(struct trkstate_t *t, byte bit, double t_bit) { // add a NRZI bit
   TRACE(data, t_bit, bit ? UPTICK : DNTICK, t);
   t->t_lastbit = t_bit;
   if (t->datacount == 0) {
      t->t_firstbit = t_bit; // record time of first bit in the datablock
      //assert(t->v_top > t->v_bot, "v_top < v_bot in nrzi_addbit at %.7lf", t_bit);
      t->max_agc_gain = t->agc_gain; }
#if 0 // we use the predefined value
   if (t->v_avg_height == 0 && bit == 1) { // make our first estimate of the peak-to-peak height
      t->v_avg_height = 2 * (max(abs(t->v_bot), abs(t->v_top))); // starting avg peak-to-peak is twice the first top or bottom
      if (t->trknum == TRACETRK)
         dlog("trk %d first avg height %.2f (v_top %.2f, v_bot %.2f) when adding %d at %.7lf\n", t->trknum, t->v_avg_height, t->v_top, t->v_bot, bit, t_bit); }
#endif
   if (!nrzi.datablock) { // this is the begining of data for this block
      nrzi.t_lastclock = t_bit - nrzi.clkavg.t_bitspaceavg;
      dlog("trk %d starts the data blk at %.7lf tick %.1lf, agc=%f, clkavg=%.2f\n",
           t->trknum, t_bit, TICK(t_bit), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6);
      nrzi.datablock = true; }
   if (trace_on)
      dlog (" [add a %d to %d bytes on trk %d at %.7lf tick %.1lf, lastpeak %.7lf tick %.1lf; now: %.7lf tick %.1lf, bitspacing %.2f, agc %.2f]\n",
            bit, t->datacount, t->trknum, t_bit, TICK(t_bit),  t->t_lastpeak, TICK(t->t_lastpeak),
            timenow, TICK(timenow), nrzi.clkavg.t_bitspaceavg*1e6, t->agc_gain);
   uint16_t mask = 1 << (ntrks - 1 - t->trknum);  // update this track's bit in the data array
   data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
   data_time[t->datacount] = t_bit;
   if (t->datacount < MAXBLOCK) ++t->datacount;
   if (nrzi.post_counter > 0 && bit) { // we're at the end of a block and get a one: must be LRC or CRC
      nrzi.t_lastclock = t_bit - nrzi.clkavg.t_bitspaceavg; // reset when we thought the last clock was
   } }

void nrzi_deletebits(void) {
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      assert(t->datacount > 0, "bad NRZI data count");
      --t->datacount; } }

void nrzi_bot(struct trkstate_t *t) { // detected a bottom
   //if (trace_on) dlog("trk %d bot at %.7f tick %.1lf, agc %.2f\n",
   //                      t->trknum, t->t_bot, TICK(t->t_bot), t->agc_gain);
   if (t->t_bot < nrzi.t_last_midbit && nrzi.post_counter == 0) {
      dlog("---trk %d bot of %.2fV at %.7lf tick %.1lf found at %.7lf tick %.1lf is %.2lfuS before midbit at %.7lf tick %.1f\n"
           "    lastclock %.7lf tick %.1f, AGC %.2f, bitspace %.2f, datacnt %d\n",
           t->trknum, t->v_bot, t->t_bot, TICK(t->t_bot), timenow, TICK(timenow), (nrzi.t_last_midbit - t->t_bot)*1e6,
           nrzi.t_last_midbit, TICK(nrzi.t_last_midbit), nrzi.t_lastclock, TICK(nrzi.t_lastclock), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6, t->datacount);
      ++block.results[block.parmset].missed_midbits; }
   nrzi_addbit(t, 1, t->t_bot);  // add a data 1
   t->hadbit = true;
   if (t->peakcount > AGC_ENDBASE && t->v_avg_height_count == 0) // if we're far enough into the data
      adjust_agc(t); }

void nrzi_top(struct trkstate_t *t) {  // detected a top
   //if (trace_on) dlog("trk %d top at %.7f tick %.1lf, agc %.2f\n",
   //                      t->trknum, t->t_top, TICK(t->t_top), t->agc_gain);
   if (t->t_top < nrzi.t_last_midbit && nrzi.post_counter == 0) {
      dlog("---trk %d top of %.2fV at %.7lf tick %.1lf found at %.7lf tick %.1lf is %.2lfuS before midbit at %.7lf tick %.1f\n"
           "    lastclock %.7lf tick %.1f, AGC %.2f, bitspace %.2f, datacnt %d\n",
           t->trknum, t->v_top, t->t_top, TICK(t->t_top), timenow, TICK(timenow), (nrzi.t_last_midbit - t->t_top)*1e6,
           nrzi.t_last_midbit, TICK(nrzi.t_last_midbit), nrzi.t_lastclock, TICK(nrzi.t_lastclock), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6, t->datacount);
      ++block.results[block.parmset].missed_midbits; }
   nrzi_addbit(t, 1, t->t_top); // add a data 1
   t->hadbit = true;
   if (t->v_top <= t->v_bot) dlog("!!! top trk %d vtop %.2f less than vbot %.2f at %.7lf tick %.1lf\n", t->trknum, t->v_top, t->v_bot, timenow, TICK(timenow));
   if (NRZI_RESET_SPEED && !nrzi.reset_speed && t->datacount == 2) { // reset speed based on the first low-to-high transition we see
      nrzi.clkavg.t_bitspaceavg = t->t_top - t->t_bot;
      for (int i = 0; i < CLKRATE_WINDOW; ++i) nrzi.clkavg.t_bitspacing[i] = nrzi.clkavg.t_bitspaceavg;
      ips = 1 / (nrzi.clkavg.t_bitspaceavg * bpi);
      nrzi.reset_speed = true;
      dlog("reset speed by trk %d to %f IPS\n", t->trknum, ips); }
   if (t->peakcount >= AGC_STARTBASE && t->peakcount <= AGC_ENDBASE) { // accumulate initial baseline peak-to-peak voltages
      t->v_avg_height_sum += t->v_top - t->v_bot;
      ++t->v_avg_height_count;
      t->v_heights[t->heightndx] = t->v_top - t->v_bot;
      if (++t->heightndx >= PARM.agc_window) t->heightndx = 0; }
   else if (t->peakcount > AGC_ENDBASE) { // we're beyond the first set of peaks and have some peak-to-peak history
      if (t->v_avg_height_count) { // if the is the first time we've gone beyond
         t->v_avg_height = t->v_avg_height_sum / t->v_avg_height_count; // then compute avg peak-to-peak voltage
         if (TRACING)
            dlog("trk %d avg peak-to-peak after %d transitions is %.2fV at %.7lf\n",  t->trknum, AGC_ENDBASE-AGC_STARTBASE, t->v_avg_height, timenow);
         assert(t->v_avg_height>0, "avg peak-to-peak voltage isn't positive");
         t->v_avg_height_count = 0; }
      else adjust_agc(t); // otherwise adjust AGC
   } }

void nrzi_midbit(void) { // we're in between NRZI bit times
   int numbits = 0;
   double avg_pos = 0;
   trace_event(trace_midbit, timenow, UPTICK, NULLP);
   nrzi.t_last_midbit = timenow;
   //if (trace_on) dlog("midbit %d at %.7lf tick %.1lf\n", nrzi.post_counter, timenow, TICK(timenow));
   if (nrzi.post_counter == 0 // if we're not at the end of the block yet
         || (nrzi.post_counter == 4 && ntrks == 9) // or we're 1.5 bit times past the CRC for 9-track tapes
         || nrzi.post_counter == 8 // or we're 1.5 bit times past the LRC for 7- or 9-track tapes
      ) {  // then process this interval
      for (int trknum = 0; trknum < ntrks; ++trknum) {
         struct trkstate_t *t = &trkstate[trknum];
         if (!t->hadbit) { // if this track had no transition at the last clock
            nrzi_addbit(t, 0, timenow - nrzi.clkavg.t_bitspaceavg * NRZI_MIDPOINT); // so add a zero bit at approximately the last clock time
         }
         else { // there was a transition on this track: accumulate the average position of the clock time
            //if (trace_on) dlog( " %d:%.1lf ", t->trknum, TICK(t->t_lastpeak));
            avg_pos += t->t_lastpeak;
            t->hadbit = false;
            ++numbits; } } // for all tracks

      if (numbits > 0) { // at least one track had a flux transition at the last clock
         //if (trace_on) dlog(" : %d track transitions\n", numbits);
         avg_pos /= numbits;  // the average real position
         trace_event(trace_avgpos, avg_pos, UPTICK, NULLP);
         double expected_pos, adjusted_pos;
         expected_pos = nrzi.t_lastclock + nrzi.clkavg.t_bitspaceavg;  // where we expected the position to be
         if (!nrzi.datablock || nrzi.post_counter > 0)
            adjusted_pos = avg_pos; // don't adjust at the beginning or in CRC/LRC territory
         else adjusted_pos = expected_pos + PARM.pulse_adj * (avg_pos - expected_pos); // adjust some amount away from the expected position
         float delta = adjusted_pos - nrzi.t_lastclock;
         if (nrzi.post_counter == 0) {
            float oldavg;
            if (DEBUG) oldavg = nrzi.clkavg.t_bitspaceavg;
            adjust_clock(&nrzi.clkavg, delta, 0);  // adjust the clock rate based on the average position
            if (DEBUG && trace_on)
               dlog("adjust clk at %.7lf tick %.1lf with delta %.2fus into avg %.2fus making %.2fus, avg pos %.7lf tick %.1lf, adj pos %.7lf tick %.1lf\n", //
                    timenow, TICK(timenow), delta*1e6, oldavg*1e6, nrzi.clkavg.t_bitspaceavg*1e6,
                    avg_pos, TICK(avg_pos), adjusted_pos, TICK(adjusted_pos)); //
         }
         nrzi.t_lastclock = adjusted_pos;  // use it as the last clock position
         if (nrzi.post_counter) ++nrzi.post_counter; // we in the post-block: must have been CRC or LRC
      }

      else { // there were no transitions, so we must be at the end of the block
         // The CRC is supposed to be after 3 quiet bit times (post_counter == 3), and then
         // the LRC is supposed to be after another 3 quiet bit times (post counter == 7).
         // The problem is that they seem often to be delayed by about half a bit, which screws up our clocking. So we:
         //  1. don't update the clock speed average during this post-data time.
         //  2. Estimate the last clock time based on any CRC or LRC 1-bit transitions we see.
         //  3. Wait until one bit time later to accmulate the 0-bits for the CRC/LRC, ie post_counter 4 and 8.
         dlog("start postcounter at %.7lf tick %.1lf\n", timenow, TICK(timenow));
         nrzi.t_lastclock += nrzi.clkavg.t_bitspaceavg; // compute a hypothetical last clock position
         if (nrzi.post_counter == 0) nrzi_deletebits(); // delete all the zero bits we just added
         ++nrzi.post_counter; } }

   else if (nrzi.post_counter) {
      ++nrzi.post_counter; // we're not processing this interval; just count it
      nrzi.t_lastclock += nrzi.clkavg.t_bitspaceavg; // and advance the hypothetical last clock position
   }

   if (nrzi.post_counter > 8) nrzi_end_of_block();
   if (trace_on) dlog("midbit at %.7lf tick %.1lf, %d transitions, lastclock %.7lf tick %.1lf, trk0 %d bytes, post_ctr %d\n", //
                         timenow, TICK(timenow), numbits, nrzi.t_lastclock, TICK(nrzi.t_lastclock),
                         trkstate[0].datacount, nrzi.post_counter); //
}
/*****************************************************************************************************************************
   Analog sample processing for all encoding types
******************************************************************************************************************************/

void end_of_block(void) {
   if (mode == PE) pe_end_of_block();
   else if (mode == NRZI) nrzi_end_of_block();
   else fatal("GCR end of block not implemented"); }

int choose_number_of_faked_bits(struct trkstate_t *t) {
   // We try various algorithms for choosing how many bits to add when there is a dropout in a track.
   // None of these is great. Have any other ideas?
   int numbits, strategy;
   int trknum = t->trknum;
   //for (strategy=1; strategy<=4; ++strategy) { // compute and display all strategies
   strategy=1; // MAKE A CHOICE HERE. Should we add it to the parameter block?
   switch(strategy) { // which of the following ideas to try
   case 1: // The number of bits is based on the time between now and the last bit, and the avg bit spacing.
      // It won't work if the clock is drifting
      numbits = (timenow - t->t_lastbit /* + t->t_bitspaceavg/2*/) / t->clkavg.t_bitspaceavg;
      break;
   case 2: //  Add enough bits to give this track the same number as the minimum of
      //  any track which is still getting data.
      numbits = INT_MAX;
      for (int i=0; i<ntrks; ++i) {
         if (i!=trknum && !trkstate[i].idle && trkstate[i].datacount < numbits) numbits = trkstate[i].datacount; }
      if (numbits != INT_MAX && numbits > t->datacount) numbits -= t->datacount;
      else numbits = 0;
      break;
   case 3:	//  Add enough bits to give this track the same number as the maximum of
      //  any track which is still getting data.
      numbits = 0;
      for (int i=0; i<ntrks; ++i) {
         if (i!=trknum && !trkstate[i].idle && trkstate[i].datacount > numbits) numbits = trkstate[i].datacount; }
      numbits -= t->datacount;
      break;
   case 4: // Add enough bits to make this track as long as the average of the other tracks
      numbits = 0;
      int numtrks = 0;
      for (int i=0; i<ntrks; ++i) {
         if (i!=trknum && !trkstate[i].idle) {
            numbits += trkstate[i].datacount;
            ++numtrks; } }
      numbits = numbits/numtrks - t->datacount;
      break;
   default: fatal("bad choose_bad_bits strategy", ""); }
   //dlog("  strategy %d would add %d bits at %.7lf\n", strategy, numbits, timenow); //
   assert(numbits>0, "choose_bad_bits bad count");
   return numbits; }

void generate_fake_bits(struct trkstate_t *t) {
   int numbits = choose_number_of_faked_bits(t);
   if (numbits > 0) {
      dlog("trk %d adding %d fake bits to %d bits at %.7lf, lastbit at %.7lf, bitspaceavg=%.2f\n", //
           t->trknum, numbits, t->datacount, timenow, t->t_lastbit, t->clkavg.t_bitspaceavg*1e6);
      if (DEBUG) show_track_datacounts("*** before adding bits");
      while (numbits--) pe_addbit(t, t->lastdatabit, true, timenow);
      t->t_lastbit = 0; // don't let the bitspacing averaging algorithm work on these bits
      if (t->lastdatabit == 0 ) { //TEMP this is bogus
         // the first new peak will be a data bit, when it comes
         t->clknext = false; }
      else { // the first new peak will be a clock bit, when it comes
         t->clknext = true;
         TRACE(clkwin, timenow, DNTICK, t); } } }

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
      dlog("trk %d not idle, %d idle at %.7f tick %.1lf, AGC %.2f, v_now %f, v_lastpeak %f, bitspaceavg %.2f\n", //
           t->trknum, num_trks_idle, timenow, TICK(timenow), t->agc_gain, t->v_now, t->v_lastpeak, t->clkavg.t_bitspaceavg*1e6); //
      if (FAKE_BITS && mode != NRZI && t->datablock && t->datacount > 1)
         //  For PE and GCR, if transitions have returned within a data block after a gap.
         //  Add extra data bits that are same as the last bit before the gap started,in an
         //  attempt to keep this track in sync with the others. .
         generate_fake_bits(t); }
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
         if (TRACING && t->datablock) {
            if (time_adjustment != 0)
               dlog("trk %d pulse adjust by %4.1f, leftdst %d, rise %.3fV, AGC %.2f, left %.3fV, peak %.3fV, right %.3fV\n",
                    t->trknum, time_adjustment, left_distance, required_rise, t->agc_gain, t->pkww_v[prevndx], val, t->pkww_v[nextndx]);
            dlog("trk %d peak of %.3fV at %.7lf tick %.1lf found at %.7lf tick %.1lf, AGC %.2f\n",
                 t->trknum, val, time, TICK(time), timenow, TICK(timenow), t->agc_gain);
            //show_window(t);//
         }
         t->pkww_countdown = left_distance;  // how long to be blind until the peak exits the window

         if (PEAK_STATS && mode == NRZI && nrzi.t_lastclock != 0  //TODO: generalize for PE
               && nrzi.datablock && nrzi.post_counter == 0) { // accumulate NRZI statistics
            if (peak_stats_leftbin == 0) { // first time: set range for statistics
               peak_stats_leftbin = t->clkavg.t_bitspaceavg - PEAK_STATS_NUMBUCKETS / 2 * PEAK_STATS_BINWIDTH;
               // round to next lower multiple of BINWIDTH
               peak_stats_leftbin = (double)(int)(peak_stats_leftbin / PEAK_STATS_BINWIDTH) * PEAK_STATS_BINWIDTH; }
            int bucket = (time - nrzi.t_lastclock - peak_stats_leftbin) / PEAK_STATS_BINWIDTH;
            bucket = max(0, min(bucket, PEAK_STATS_NUMBUCKETS - 1));
            ++peak_counts[t->trknum][bucket];
            ++peak_trksums[t->trknum]; }

         return time; }
      ++left_distance;
      if (ndx == t->pkww_right) break;
      prevndx = ndx;
      if (++ndx >= pkww_width) ndx = 0; }
   fatal( "Can't find max or min %f in trk %d window at time %.7lf", val, t->trknum, timenow);
   return 0; }

// Process one voltage sample with data for all tracks.
// Return with the status of the block we're working on.

enum bstate_t process_sample(struct sample_t *sample) {
   float deltaT;

   if (sample == NULLP) { // special code for endfile
      end_of_block();
      return BS_NONE; }

   deltaT = sample->time - timenow;  //(incorrect for the first sample)
   timenow = sample->time;

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
   trace_newtime(timenow, deltaT, sample, &trkstate[TRACETRK]);
   // or create some number of evenly-spaced trace entries for this sample, for more accuracy
   //trace_newtime(timenow, sample_deltat / 2, sample, &trkstate[TRACETRK]);
   //trace_newtime(timenow + sample_deltat / 2, sample_deltat / 2, sample, &trkstate[TRACETRK]);
#endif

   if (interblock_expiration && timenow < interblock_expiration) // if we're waiting for an IBG to really start
      goto exit;

   if (nrzi.datablock && timenow > nrzi.t_lastclock + (1 + NRZI_MIDPOINT)*nrzi.clkavg.t_bitspaceavg) {
      nrzi_midbit();  // do NRZI mid-bit computations
   }

   for (int trknum = 0; trknum < ntrks; ++trknum) {  // look at the analog signal on each track
      struct trkstate_t *t = &trkstate[trknum];
#if !DESKEW
      t->v_now = sample->voltage[trknum]; // no deskewing was done in preprocessing
#endif
      // Zero-banding doesn't work because jitter near the band edge causes false peaks
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
         float required_rise = PKWW_RISE * (t->v_avg_height / PKWW_PEAKHEIGHT) / t->agc_gain;  // how much of a voltage rise constitutes a peak
         //if (TRACING) dlog("trk %d at %.7lf req rise %f, avg height %.2f, AGC %.2f\n", t->trknum, timenow, required_rise, t->v_avg_height, t->agc_gain);

         if (t->pkww_maxv > t->pkww_v[t->pkww_left] + required_rise
               && t->pkww_maxv > t->pkww_v[t->pkww_right] + required_rise  // the max is a lot higher than the left and right sides
               && (PARM.min_peak == 0 || t->pkww_maxv > PARM.min_peak)) {  // and it's higher than the min peak, if given
            t->v_top = t->pkww_maxv;  // which means we hit a top peak
            t->t_top = process_peak(t, t->pkww_maxv, true, required_rise);
            //if (TRACING) dlog("hit top on trk %d of %.2fV at %.7lf tick %.1f \n", t->trknum, t->v_top, t->t_top, TICK(t->t_top));
            TRACE(peak, t->t_top, UPTICK, t);
            ++t->peakcount;
            if (doing_density_detection)
               estden_transition(t->t_top - t->t_lastpeak);
            else {
               if (mode == PE) pe_top(t);
               else if (mode == NRZI) nrzi_top(t);
               else fatal("GCR not supported yet"); }
            t->v_lasttop = t->v_top;
            t->v_lastpeak = t->v_top;
            t->t_lastpeak = t->t_top; }

         else if (t->pkww_minv < t->pkww_v[t->pkww_left] - required_rise
                  && t->pkww_minv < t->pkww_v[t->pkww_right] - required_rise  // the min is a lot lower than the left and right sides
                  && (PARM.min_peak == 0 || t->pkww_minv < -PARM.min_peak)) { // and it's lower than the min peak, if given
            t->v_bot = t->pkww_minv;  //so we hit a bottom peak
            t->t_bot = process_peak(t, t->pkww_minv, false, required_rise);
            //if (TRACING) dlog("hit bot on trk %d of %.2fV at %.7lf tick %.1f \n", t->trknum, t->v_bot, t->t_bot, TICK(t->t_bot));
            TRACE(peak, t->t_bot, DNTICK, t);
            ++t->peakcount;
            if (doing_density_detection)
               estden_transition(t->t_bot - t->t_lastpeak);
            else {
               if (mode == PE) pe_bot(t);
               else if (mode == NRZI) nrzi_bot(t);
               else fatal("GCR not supported yet"); }
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
         if (++num_trks_idle >= IDLE_TRK_LIMIT) {
            end_of_block(); } }

   } // for tracks

#if TRACEFILE
// Choose a test here for turning the trace on, depending on what anomaly we're looking at...
//if (timenow > 8.26944
//if (timenow > 13.1369 && trkstate[0].datacount > 270
//if (num_samples == 8500
//if (trkstate[TRACETRK].peakcount > 70
   if (trkstate[TRACETRK].datacount > 0
         //if (trkstate[5].v_now > 0.5
         //if (nrzi.clkavg.t_bitspaceavg > 40e-6
         //if (nrzi.datablock
         && !doing_deskew && !trace_on && !trace_done) {
      trace_open();
      trace_on = true;
      torigin = timenow - sample_deltat;
      dlog("-----> trace started at %.7lf tick %.1lf\n", timenow, TICK(timenow)); }
   if (trace_on && ++trace_lines > 10000) { // limit on how much trace data to collect
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

