//file: decoder.c
/*****************************************************************************

Decode analog magnetic tape data in one of several flavors:
  -- Manchester phase encoded (PE)
  -- NRZI (non-return-to-zero inverted) encoded,
     including the 6-track 100 BPI Whirlwind variant
  -- GCR group coded recording

We are called once for each set of read head voltages on all data tracks.

Each track is processed independently, so that head and data skew
(especially for PE and GCR, which are self-clocking) is irrelevant.
We look for relative minima and maxima of the head voltage that
represent the downward and upward flux transitions, or we
look for when the signal crosses zero in either direction.

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

#include "decoder.h"

// Variables for time relative to the start must be at least double-precision.
// Delta times for samples close to each other can be single-precision,
// although the compiler might complain about loss of precision.
// Time calculations that are subject to cumulative errors must be done
// in 64-bit integer number of nanoseconds.

int num_trks_idle;
int errcode=0;
struct nrzi_t nrzi = { 0 };      // NRZI decoding status
struct ww_t ww = { 0 };          // Whirlwind decoding status
int pkww_width = 0;              // the width of the peak detection window, in number of samples

double timenow = 0;              // time of last sample in seconds
int64_t timenow_ns = 0;          // time of last sample in nanoseconds (used when reading .tbin files)
double torigin = 0;              // time origin for debugging displays, in seconds
float sample_deltat = 0;         // time between samples, in seconds
int64_t sample_deltat_ns = 0;    // time between samples, in nanoseconds
int interblock_counter = 0;      // sample countdown to get us well into the interblock gap

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
float peak_stats_leftbin;
float peak_stats_binwidth;
int peak_counts[MAXTRKS][PEAK_STATS_NUMBUCKETS];
int peak_trksums[MAXTRKS]; // all counts other than at the two extremes
bool peak_stats_initialized = false;

float peak_block_deviation[MAXTRKS]; // avg deviation from the expected peak, for adjdeskew
int peak_block_counts[MAXTRKS];

void reset_peak_blockcounts(void) {
   memset(peak_block_deviation, 0, sizeof(peak_block_deviation));
   memset(peak_block_counts, 0, sizeof(peak_block_counts)); }


void record_peakstat(float bitspacing, float peaktime, int trknum) {
   if (!peak_stats_initialized) { // first time: set range for statistics
      peak_stats_leftbin = peak_stats_binwidth = 0;
      memset(peak_counts, 0, sizeof(peak_counts));
      memset(peak_trksums, 0, sizeof(peak_trksums));
      reset_peak_blockcounts();
      // we want bins for a half bit time below the expected peak(s), and a half bit time above
      float range = bitspacing *
                    (mode == NRZI ? 1.0f
                     : mode == PE ? 1.2f
                     : mode == GCR ? 3.0f
                     : mode == WW ? 0.75f
                     : 1.0f);
      peak_stats_binwidth = range / PEAK_STATS_NUMBUCKETS;
      // round to the nearest 0.1 usec so numbers print nicely
      peak_stats_binwidth = (float)((int)(peak_stats_binwidth*10e6 + 0.5)*1e-6) / 10.0f;
      //peak_stats_leftbin = mode == PE ? bitspacing * 0.25f : bitspacing * 0.5f;
      peak_stats_leftbin = bitspacing - range / 2;
      // round to next lower multiple of BINWIDTH
      peak_stats_leftbin = (float)(int)(peak_stats_leftbin / peak_stats_binwidth) * peak_stats_binwidth;
      dlog("peakstats started: bitspacing %.2f, peaktime %.2f range %.2f binwidth %.2f leftbin %.2f\n",
           bitspacing*1e6, peaktime*1e6, range*1e6, peak_stats_binwidth*1e6, peak_stats_leftbin*1e6);
      peak_stats_initialized = true; }
   int bucket = (int)((peaktime - peak_stats_leftbin) / peak_stats_binwidth);
   if (bucket < 0) {
      ++peak_counts[trknum][0];
      //rlog("left bin: spacing %.1f, peaktime %.1f, trk %d time "TIMEFMT"\n",
      //   bitspacing*1e6, peaktime*1e6, trknum, TIMETICK(timenow)); // TEMP
   }
   else if (bucket >= PEAK_STATS_NUMBUCKETS) ++peak_counts[trknum][PEAK_STATS_NUMBUCKETS - 1];
   else { // not one of the extremes
      ++peak_counts[trknum][bucket];
      ++peak_trksums[trknum];
      if (adjdeskew) {  // keep track of the average deviations of this track's peaks for this block
         // This is the semi-naive incremental average calculation: A(n+1) = A(n) + (V(n+1) - A(n)) / (n+1)
         // For a more sophisticated algorithm, see the code for my microammeter.
         ++peak_block_counts[trknum];
         peak_block_deviation[trknum] = peak_block_deviation[trknum] + ((peaktime - bitspacing) - peak_block_deviation[trknum]) / peak_block_counts[trknum]; } } }

void output_peakstats(const char *name) { // Create an Excel .CSV file with flux transition position statistics
   FILE *statsf;
   char filename[MAXPATH];
   int trk, bkt;
   int totalcount = 0;
   long long int avgsum;
   sprintf(filename, "%s.peakstats%s.csv", baseoutfilename, name);
   assert((statsf = fopen(filename, "w")) != NULLP, "can't open stats file \"%s\"", filename);
   fprintf(statsf, "total cnt, <=%.1f uS, >=%.1f uS, track",  // first 3 columns are counts of totals, and the 2 extreme buckets
           peak_stats_leftbin * 1e6, peak_stats_binwidth*1e6 * (PEAK_STATS_NUMBUCKETS-1) + peak_stats_leftbin * 1e6);
   for (bkt = 1; bkt < PEAK_STATS_NUMBUCKETS-1; ++bkt)
      fprintf(statsf, ",%.1f uS", peak_stats_binwidth*1e6 * bkt + peak_stats_leftbin * 1e6);
   if (mode == NRZI) fprintf(statsf, ",avg uS");
   fprintf(statsf, "\n");
#if (0) // dump raw numbers for debugging
   for (trk = 0; trk < ntrks; ++trk) {
      fprintf(statsf, ", %d, %d, trk%d", trk, peak_counts[trk][0], peak_counts[trk][PEAK_STATS_NUMBUCKETS - 1]);
      for (bkt = 1; bkt < PEAK_STATS_NUMBUCKETS-1; ++bkt)
         fprintf(statsf, ", %d", peak_counts[trk][bkt]);
      fprintf(statsf, ", %d\n", peak_trksums[trk]); }
   fprintf(statsf, "\n");
#endif
   for (trk = 0; trk < ntrks; ++trk) { // show percentages
      avgsum = 0;
      // we don't count the first/last buckets, which catch the extremes
      for (bkt = 1; bkt < PEAK_STATS_NUMBUCKETS-1; ++bkt) {
         avgsum += (long long)(peak_counts[trk][bkt] * (peak_stats_binwidth*1e6 * bkt + peak_stats_leftbin * 1e6)); }
      fprintf(statsf, "%d, %d, %d,", peak_trksums[trk] + peak_counts[trk][0] + peak_counts[trk][PEAK_STATS_NUMBUCKETS - 1],
              peak_counts[trk][0], peak_counts[trk][PEAK_STATS_NUMBUCKETS - 1]);
      fprintf(statsf, "trk%d", trk);
      for (bkt = 1; bkt < PEAK_STATS_NUMBUCKETS-1; ++bkt)
         fprintf(statsf, ", %.2f%%", peak_trksums[trk] ? 100 * (float)peak_counts[trk][bkt] / (float)peak_trksums[trk] : 0);
      if (mode == NRZI) fprintf(statsf, ", %.2f", (float)avgsum / (float)peak_trksums[trk]);
      fprintf(statsf, "\n");
      totalcount += peak_trksums[trk]; }
   fclose(statsf);
   if (!quiet) {
      rlog("  created statistics file \"%s\" from %s measurements of flux transition positions\n", filename, intcommas(totalcount));
      rlog("  to graph it from Excel, open the CSV file, then: insert chart 2D line\n"); }
   peak_stats_initialized = false; }

#endif

/***********************************************************************************************************************
   Routines for using accumulated flux transition times to
   deskew the input data by delaying some of the channel data

We do this for NRZI, GCR, and Whirlwind, where the tracks must be aligned.
PE tracks are self-clocking, so track skew isn't a problem.
************************************************************************************************************************/

#if DESKEW
struct skew_t {
   float vdelayed[MAXSKEWSAMP]; // the buffered voltages
   int ndx_next;            // the next slot to use for the newest data
   int slots_filled;        // how many slots are filled, 0..skewdelaycnt[trk]
} skew[MAXTRKS];            // (This structure is cleared at the start of each block.)
int skew_delaycnt[MAXTRKS] = { 0 };    // the skew delay, in number of samples, for each track. (This is persistent.)
float deskew_max_delay_percent;       // the worse-case percentage-of-a-bit skew delay

void skew_set_delay(int trknum, float time) { // set the skew delay for a track
   assert(sample_deltat > 0, "delta T not set yet in skew_set_delay");
   assert(time >= 0, "negative skew amount %f for trk %d which had delaycnt %d", time, trknum, skew_delaycnt[trknum]);
   int delay = (int)((time + sample_deltat/2) / sample_deltat);
   //rlog("set skew trk %d to %f usec, %d samples\n", trknum, time*1e6, delay);
   if (delay > MAXSKEWSAMP) rlog("---> Warning: head %d skew of %.1f usec is too big\n", trknum, time*1e6);
   skew_delaycnt[trknum] = min(delay, MAXSKEWSAMP); };

bool skew_compute_deskew(bool do_set) {
   // compute (and optionally set) deskew amounts based on where we see the average
   // of the transitions for each track. Return true if the skew is within acceptable bounds.
   int trk, bkt;
   long long int avgsum;
   float avg[MAXTRKS]; // the average peak position for each track
   float stddev[MAXTRKS]; // the std deviation of the peak positions for each track
   // we don't count the first/last buckets, which catch the extremes
   for (trk = 0; trk < ntrks; ++trk) { // compute the average peak positions
      avgsum = 0;
      for (bkt = 1; bkt < PEAK_STATS_NUMBUCKETS-1; ++bkt) {
         avgsum += (long long) (peak_counts[trk][bkt] * (peak_stats_binwidth*1e6 * bkt + peak_stats_leftbin * 1e6)); }
      avg[trk] = (float)avgsum / (float)peak_trksums[trk]; }
   for (trk = 0; trk < ntrks; ++trk) { // compute the standard deviations of the peak positions
      stddev[trk] = 0;
      for (bkt = 1; bkt < PEAK_STATS_NUMBUCKETS - 1; ++bkt) {
         float deviation = ((peak_stats_binwidth*1e6f * bkt + peak_stats_leftbin * 1e6f)) - avg[trk];
         stddev[trk] += peak_counts[trk][bkt] * (deviation * deviation); }
      stddev[trk] = sqrtf(stddev[trk]/(float)peak_trksums[trk]); }
   float maxavg = 0, minavg = FLT_MAX, maxstddev = 0;;
   for (trk = 0; trk < ntrks; ++trk) { // find min/max average, and max std deviation
      maxavg = max(maxavg, avg[trk]);
      minavg = min(minavg, avg[trk]);
      maxstddev = max(maxstddev, stddev[trk]); }
   if (do_set) {
      for (trk = 0; trk < ntrks; ++trk) {
         dlog("trk %d has %d transitions, avg position %.2f usec, std dev %.2f usec\n", trk, peak_trksums[trk], avg[trk], stddev[trk]);
         skew_set_delay(trk, peak_trksums[trk] > 0 ? (maxavg - avg[trk]) / 1e6f : 0); // delay the other tracks relative to it
      }
      if (!quiet) skew_display(); }
   float peak_frac = (maxavg - minavg) / (1e6f / (bpi*ips)); // peak difference as a fraction of bit spacing
   float stddev_frac = maxstddev / (1e6f / (bpi*ips)); // standard deviation as a fraction of bit spacing
   if (!quiet) {
      rlog("  the earliest peak is %.2f usec, and the latest peak is %.2f usec\n",
           minavg, maxavg);
      rlog("  that peak difference of %.2f usec, and the largest standard deviation of %.2f usec, are %.1f%% and %.1f%% of the nominal bit spacing\n",
           maxavg - minavg, maxstddev, peak_frac * 100, stddev_frac * 100); }
   if (do_set) deskew_max_delay_percent = peak_frac * 100; // save what we set it to in a global for reporting later
   return peak_frac < DESKEW_PEAKDIFF_WARNING && stddev_frac < DESKEW_STDDEV_WARNING; }

int skew_min_transitions(void) { // return the lowest number of transitions on any track
   int min_transitions = INT_MAX;
   for (int trknum = 0; trknum < ntrks; ++trknum)
      if (peak_trksums[trknum] < min_transitions) min_transitions = peak_trksums[trknum];
   return min_transitions; }

void skew_display(void) { // show the track skews
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      rlog("  track %d delayed by %d clocks (%.2f usec) ",
           trknum, skew_delaycnt[trknum], skew_delaycnt[trknum] * sample_deltat * 1e6);
      if (skew_given) rlog("as specified by \"skew=\"\n");
      else rlog("based on %d observed flux transitions\n", peak_trksums[trknum]); } }

void skew_display_history(int trknum) {
   rlog(" track %d skew history: ", trknum);
   int ndx = skew[trknum].ndx_next; // pointer to oldest
   do {
      rlog("%.3fV ", skew[trknum].vdelayed[ndx]);
      if (++ndx >= skew_delaycnt[trknum]) ndx = 0; }
   while (ndx != skew[trknum].ndx_next);
   rlog("\n"); }

// The following code to implement adjdeskew is experimental and doesn't work yet
void adjust_deskew(float bitspacing) { // slowly adjust the deskew based on the peaks we saw in the previous block
#define ADJ_DESKEW_THRESHOLD 0.1f  // fraction of a bit that the average needs to exceed
   // This can only be called when we expect no peaks in the near future
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      float deviation = peak_block_deviation[trknum];
      rlog("trk %d deviation is %.2f usec of bitspacing %.2f usec", trknum, deviation*1e6, bitspacing*1e6);
      if (deviation < ADJ_DESKEW_THRESHOLD * bitspacing && skew_delaycnt[trknum] > 0) {
         --skew_delaycnt[trknum];
         rlog(", skew reduced to %d", skew_delaycnt[trknum]); }
      else if (deviation > ADJ_DESKEW_THRESHOLD * bitspacing && skew_delaycnt[trknum] < MAXSKEWSAMP) {
         ++skew_delaycnt[trknum];
         rlog(", skew increased to %d",skew_delaycnt[trknum]); }
      rlog("\n"); }
   reset_peak_blockcounts(); // get ready for next block
}
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

bool estden_transition(struct trkstate_t *t, double peaktime, float deltasecs) { // count a transition distance
   int delta = (int) (deltasecs / ESTDEN_BINWIDTH);  // round down to multiple of BINWIDTH
   int ndx;
   //dlog("estden_transition %.3f usec at %.8lf\n", deltasecs*1e6, timenow);
   assert(deltasecs > 0, "negative delta %f usec in estden_transition", deltasecs*1e6);
   if (deltasecs > 0 && deltasecs <= ESTDEN_MAXDELTA) {
      //if (deltasecs < 2e-6)dlog("estden_transition %f usec trk %d thispeak %.8lf tick %.1lf, lastpeak %.8lf tick %.1lf, at %.8lf tick %.1lf\n",
      //   deltasecs*1e6, t->trknum, peaktime, TICK(peaktime), t->t_lastpeak, TICK(t->t_lastpeak), timenow, TICK(timenow));
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
      rlog(" %2d: count %5d, %f usec\n", ndx, estden.counts[ndx], estden.deltas[ndx] * ESTDEN_BINWIDTH * 1e6); }

void estden_setdensity(int nblks) { // figure out the tape density
   //estden_show();
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
         if (!quiet) rlog("  density was set to %.0f BPI (%.2f usec/bit) after reading the first %d blocks and seeing %s transitions in %d bins that imply %.0f BPI\n",
                             bpi, 1e6/(bpi*ips), nblks, intcommas(estden.totalcount), estden.binsused, density);
         return; } }
   fatal("The detected density of %.0f (%.1f usec) after seeing %s transitions is non-standard; please specify it.",
         density, (float)(mindist + 0.5) * ESTDEN_BINWIDTH * 1e6, intcommas(estden.totalcount)); }


/*****************************************************************************************************************************
   Routines used for all encoding types
******************************************************************************************************************************/

void init_blockstate(void) {	// initialize block state information for multiple reads of a block
   for (int parmndx=0; parmndx < MAXPARMSETS; ++parmndx) {
      assert(parmsetsptr[parmndx].active == 0 || strcmp(parmsetsptr[parmndx].id, "PRM") == 0, "bad parm block initialization");
      memset(&block.results[parmndx], 0, sizeof(struct results_t));
      block.results[parmndx].blktype = BS_NONE; } }

void init_clkavg(struct clkavg_t *c, float init_avg) { // initialize a clock averaging structure
   c->t_bitspaceavg = init_avg;
   c->bitndx = 0;
   for (int i = 0; i < CLKRATE_WINDOW; ++i) // initialize moving average bitspacing array
      c->t_bitspacing[i] = init_avg; }

void init_trackpeak_state(void) { // this is also used by Whirlwind when we move back in the file
#if DESKEW
   memset(&skew, 0, sizeof(skew));
#endif
   block.window_set = false;
   block.endblock_done = false;
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *trk = &trkstate[trknum];
      trk->pkww_left = trk->pkww_right = 0;
      trk->pkww_minv = trk->pkww_maxv = 0;
      trk->pkww_countdown = 0; } }

void init_trackstate(void) {  // initialize all track and some block state information for a new decoding of a block
   // for Whirlwind we do this only once at the beginning, because we need to preseve peak height info for very close blocks
   num_trks_idle = ntrks;
   block.window_set = false;
   block.endblock_done = false;
   expected_parity = specified_parity;
   if (mode == GCR) gcr_preprocess();
   init_trackpeak_state();
   memset(&block.results[block.parmset], 0, sizeof(struct results_t));
   block.results[block.parmset].blktype = BS_NONE;
   block.results[block.parmset].alltrk_max_agc_gain = 0.0;
   block.results[block.parmset].alltrk_min_agc_gain = FLT_MAX;
   memset(trkstate, 0, sizeof(trkstate));  // only need to initialize non-zeros below
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *trk = &trkstate[trknum];
      trk->trknum = trknum;
      trk->idle = true;
      trk->v_last_raw = 0;
      //  trk->zerocross_dn_pending = trk->zerocross_up_pending = true; // allow first zerocrossing to occur from an idle track
      trk->agc_gain = 1.0;
      trk->max_agc_gain = 0.0;
      trk->min_agc_gain = FLT_MAX;
      trk->v_avg_height = PKWW_PEAKHEIGHT;
      if (!doing_density_detection) init_clkavg(&trk->clkavg, 1 / (bpi*ips));
      trk->t_clkwindow = trk->clkavg.t_bitspaceavg / 2 * PARM.clk_factor; }
   if (mode == NRZI) {
      memset(&nrzi, 0, sizeof(nrzi));
      if (!doing_density_detection) init_clkavg(&nrzi.clkavg, 1 / (bpi*ips)); }
   if (mode == WW) {
      memset(&ww, 0, sizeof(ww));
      if (!doing_density_detection) init_clkavg(&ww.clkavg, 1 / (bpi*ips)); } }

void set_expected_parity(int blklength) {
   expected_parity =
      blklength > 0 && blklength == revparity ? 1 - specified_parity
      : specified_parity;
   //rlog("set_expected_parity(%d) = %d, revparity %d specified_parity %d\n",
   //  blklength, expected_parity, revparity, specified_parity);
}

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
         dlog("! at clk on trk %d, trk %d has %d databytes, not %d, at %.8lf\n", clktrk, trknum, t->datacount, datacount, timenow);
         ++numshown; } } }

// the following avg height routines are new for WW, and NRZI/PE/GCR ought to use them too instead of having their own code for this

void accumulate_avg_height(struct trkstate_t *t) {
   if (t->v_top > t->v_bot) {
      t->v_avg_height_sum += t->v_top - t->v_bot;
      ++t->v_avg_height_count;
      t->v_heights[t->heightndx] = t->v_top - t->v_bot;
      if (++t->heightndx >= PARM.agc_window) t->heightndx = 0; } }

void compute_avg_height(struct trkstate_t *t) {
   if (t->v_avg_height_count) {
      t->v_avg_height = t->v_avg_height_sum / t->v_avg_height_count; // then compute avg peak-to-peak voltage
      dlogtrk("trk %d avg peak-to-peak after %d transitions is %.2fV at "TIMEFMT"\n",
              t->trknum, t->v_avg_height_count, t->v_avg_height, TIMETICK(timenow));
      assert(t->v_avg_height>0, "avg peak-to-peak voltage isn't positive");
      t->v_avg_height_count = 0;  t->v_avg_height_sum = 0; } }


void adjust_agc(struct trkstate_t *t) { // update the automatic gain control level
   if (find_zeros) return;  // no AGC if we're looking for zero crossings instead of peaks
   assert(!PARM.agc_window || !PARM.agc_alpha, "inconsistent AGC parameters in parmset %d", block.parmset);
   float gain, lastheight;
   if (PARM.agc_alpha) {  // do automatic gain control based on exponential averaging
      lastheight = t->v_lasttop - t->v_lastbot; // last peak-to-peak height
      if (lastheight > 0 /*&& lastheight < t->v_avg_height*/ ) { // if it's smaller than average  *** NO, DO THIS ALWAYS??
         gain = t->v_avg_height / lastheight;  		// the new gain, which could be less than 1
         gain = PARM.agc_alpha * gain + (1 - PARM.agc_alpha)*t->agc_gain;  // exponential smoothing with previous values
         if (gain > AGC_MAX_VALUE) gain = AGC_MAX_VALUE;
         //dlogtrk("trk %d adjust gain lasttop %.2f lastbot %.2f lastheight %.2f, avgheight %.2f, old gain %.2f new gain %.2f at %.8lf tick %.1lf\n",
         //        t->trknum, t->v_lasttop, t->v_lastbot, lastheight, t->v_avg_height, t->agc_gain, gain, timenow, TICK(timenow));
         t->agc_gain = gain;
         if (gain > t->max_agc_gain) t->max_agc_gain = gain;
         if (gain < t->min_agc_gain) t->min_agc_gain = gain; } }
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
            if (gain > AGC_MAX_VALUE) gain = AGC_MAX_VALUE;
            t->agc_gain = gain;
            // dlogtrk("adjust_gain: trk %d lastheight %.3fV, avgheight %.3f, minheight %.3f, heightndx %d, datacount %d, gain is now %.3f at %.8lf\n",
            //        t->trknum, lastheight, t->v_avg_height, minheight, t->heightndx, t->datacount, gain, timenow);
            if (gain > t->max_agc_gain) t->max_agc_gain = gain;
            if (gain < t->min_agc_gain) t->min_agc_gain = gain; } } } }

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
      assert(bpi > 0, "bpi=0 in adjust_clock at %.8lf", timenow);
      c->t_bitspaceavg = mode & PE+WW ? 1 / (bpi*ips) : nrzi.clkavg.t_bitspaceavg; //
   }
   //if (DEBUG && trace_on && (DEBUGALL || trk == TRACETRK))
      //rlog("trk %d adjust clock of %.2f with delta %.2f uS to %.2f at %.8lf tick %.1lf\n",
      //     trk, prevdelta*1e6, delta*1e6, c->t_bitspaceavg*1e6, timenow, TICK(timenow)); //
   }
void force_clock(struct clkavg_t *c, float delta, int trk) { // force the clock speed
   for (int i = 0; i < CLKRATE_WINDOW; ++i) c->t_bitspacing[i] = delta;
   c->t_bitspaceavg = delta; }

void process_transition(struct trkstate_t *t) {  // process a transition: a zero-crossing, or peak
   ++t->peakcount;
   if (t->idle) { // we're coming out of idle
      --num_trks_idle;
      t->idle = false;
      //dlog("trk %d is #%d not idle at %.7f tick %.1lf, AGC %.2f, v_now %f, v_lastpeak %f at %.8lf tick %.1lf, bitspaceavg %.2f\n", //
      //     t->trknum, ntrks - num_trks_idle, timenow, TICK(timenow), t->agc_gain, t->v_now,
      //     t->v_lastpeak, t->t_lastpeak, TICK(t->t_lastpeak), t->clkavg.t_bitspaceavg*1e6); //
      if (FAKE_BITS && mode == PE && t->datablock && t->datacount > 1)
         //  For PE, if transitions have returned within a data block after a gap.
         //  Add extra data bits that are same as the last bit before the gap started,in an
         //  attempt to keep this track in sync with the others. .
         pe_generate_fake_bits(t); } }

void process_up_transition(struct trkstate_t *t) {
   //rlog("up transition on trk %d at %.8lf\n", t->trknum, t->t_top);
   TRACE(peak, t->t_top, UPTICK, t);
   process_transition(t);
   if (doing_density_detection) {
      if (estden_transition(t, t->t_top, (float)(t->t_top - t->t_lastpeak)))
         block.results[block.parmset].blktype = BS_ABORTED; // got enough transitions for density detect
   }
   else {
      if (mode == PE) pe_top(t);
      else if (mode == NRZI) nrzi_top(t);
      else if (mode == GCR) gcr_top(t);
      else if (mode == WW) ww_top(t); }
   t->v_lasttop = t->v_top;
   t->v_lastpeak = t->v_top;
   t->t_prevlastpeak = t->t_lastpeak;
   t->t_lastpeak = t->t_top; }

void process_down_transition(struct trkstate_t *t) {
   //rlog("dn transition on trk %d at %.8lf\n", t->trknum, t->t_bot);
   TRACE(peak, t->t_bot, DNTICK, t);
   process_transition(t);
   if (doing_density_detection) {
      if (estden_transition(t, t->t_bot, (float)(t->t_bot - t->t_lastpeak)))
         block.results[block.parmset].blktype = BS_ABORTED; // got enough transitions for density detect
   }
   else {
      if (mode == PE) pe_bot(t);
      else if (mode == NRZI) nrzi_bot(t);
      else if (mode == GCR) gcr_bot(t);
      else if (mode == WW) ww_bot(t); }
   t->v_lastbot = t->v_bot;
   t->t_lastbot = t->t_bot;
   t->v_lastpeak = t->v_bot;
   t->t_prevlastpeak = t->t_lastpeak;
   t->t_lastpeak = t->t_bot; }

//***** routines that look for a zero crossing using trivial algorithms that assume minimal jitter *****

// In this algorithm we keep track of the maximum excursions above or below zero, and make sure
// that it is larger than ZEROCROSS_PEAK before we are willing to record a zero crossing,
// and that the crossing happened quickly enough to be a real transition.

void lookfor_zerocrossing(struct trkstate_t *t) {
   if (0 && t->trknum == TRACETRK && timenow >= 0.8608976)
      rlog("zerochk: v_now=%.3f, v_top=%.3f, t_top=%.8lf, v_bot=%.3f, t_bot=%.8lf, up_pending=%d, dn_pending=%d, tnow=%.8lf\n",
           t->v_now, t->v_top, t->t_top, t->v_bot, t->t_bot, t->zerocross_up_pending, t->zerocross_dn_pending, timenow);
   if (t->v_now > 0) { // voltage is above zero
      t->zerocross_dn_pending = false;  // cancel any pending down transition
      if (t->v_top < t->v_now) {  // new maximum above zero
         t->v_top = t->v_now;
         if (t->zerocross_up_pending && t->v_top > ZEROCROSS_PEAK) { // can record the previous zero crossing up
            if (t->t_top == 0) t->t_top = timenow;  // (only happens the first time)
            t->zerocross_up_pending = false;
            t->v_bot = 0; // reset bottom excursion min
            if (timenow - t->t_top <= t->clkavg.t_bitspaceavg * ZEROCROSS_SLOPE) // only if the min excursion happened soon enough
               process_up_transition(t); } }
      if (t->v_prev < 0 && t->v_bot < -ZEROCROSS_PEAK) { // just crossed zero going up after a big down peak
         t->t_top = timenow; // remember this as a possible zero crossing up
         //if (t->trknum == TRACETRK) rlog("trk %d zerocross up pending at %.8lf, v_bot %.3f\n", t->trknum, timenow, t->v_bot);
         t->zerocross_up_pending = true; } }
   else if (t->v_now < 0) { // voltage is below zero
      t->zerocross_up_pending = false; // cancel any pending up transition
      if (t->v_bot > t->v_now) { // new minimum below zero
         t->v_bot = t->v_now;
         if (t->zerocross_dn_pending && t->v_bot < -ZEROCROSS_PEAK) { // can record the previous zero crossing down
            if (t->t_bot == 0) t->t_bot = timenow;  // (only happens the first time)
            t->zerocross_dn_pending = false;
            t->v_top = 0; // reset top excursion max
            if (timenow - t->t_bot <= t->clkavg.t_bitspaceavg * ZEROCROSS_SLOPE) // only if the min excursion happened soon enough
               process_down_transition(t); } }
      if (t->v_prev > 0 && t->v_top > ZEROCROSS_PEAK ) { // just crossed zero going down after a big up peak
         t->t_bot = timenow; // remember this as a possible zero crossing down
         //if (t->trknum == TRACETRK) rlog("trk %d zerocross dn pending at %.8lf, v_top %.3f\n", t->trknum, timenow, t->v_top);
         t->zerocross_dn_pending = true; } }
   t->v_prev = t->v_now; }

// We use this different algorithm for detecting zero crossings after we have differentiated a
// relatively clean signal, when small deltas are forced to be zero.

void lookfor_differentiated_zerocrossing(struct trkstate_t *t) {
   if (t->v_now > 0) { // voltage is above zero
      if (t->v_top < t->v_now)  t->v_top = t->v_now;  // new maximum above zero
      if (t->zerocross_up_pending) { // we were waiting for zerocrossing up and just got it
         t->t_top = t->t_firstzero > 0 // we saw one or more actual zeros
                    ? (t->t_firstzero + t->t_lastzero)/2  // so pick the center time of all of them
                    : timenow - sample_deltat/2; // otherwise use the midpoint of the 2 samples that straddle (could interpolate)
         t->zerocross_up_pending = false;
         t->t_firstzero = 0;
         process_up_transition(t); }
      if (t->v_now > ZEROCROSS_PEAK) { // we're high enough to wait for a zerocrossing down
         t->zerocross_dn_pending = true;
         t->t_firstzero = 0;
         t->v_bot = 0; } }
   else if (t->v_now < 0) { // voltage is below zero
      if (t->v_bot > t->v_now)  t->v_bot = t->v_now;  // new minimum below zero
      if (t->zerocross_dn_pending) { // we were waiting for zerocrossing down and just got it
         t->t_bot = t->t_firstzero > 0  // we saw one or more actual zeros
                    ? (t->t_firstzero + t->t_lastzero)/2  // so pick the center time of all of them
                    : timenow - sample_deltat/2; // otherwise use the midpoint of the 2 samples that straddle (could interpolate)
         t->zerocross_dn_pending = false;
         t->t_firstzero = 0;
         process_down_transition(t); }
      if (t->v_now < -ZEROCROSS_PEAK) {  // we're low enough to wait for a zerocrossing up
         t->zerocross_up_pending = true;
         t->t_firstzero = 0;
         t->v_top = 0; } }
   else { // we have (another?) true zero: keep track of the first and last of them
      t->t_lastzero = timenow;
      if (t->t_firstzero == 0) t->t_firstzero = timenow; } }

//****** routines that look for a peak using the moving-window algorithm

void show_window(struct trkstate_t *t) { // for debugging
   rlog("trk %d window at %.8lf after adding %f, left=%d, right=%d, countdown=%d:\n",
        t->trknum, timenow, t->v_now, t->pkww_left, t->pkww_right, t->pkww_countdown);
   for (int ndx = t->pkww_left; ; ) {
      rlog(" %f", t->pkww_v[ndx]);
      if (t->pkww_v[ndx] == t->pkww_minv) rlog("m");
      if (t->pkww_v[ndx] == t->pkww_maxv) rlog("M");
      if (ndx == t->pkww_right) break;
      if (++ndx >= pkww_width) ndx = 0; }
   rlog("\n"); }

extern bool rereading; //TEMP

double refine_peak (struct trkstate_t *t, float val, bool top, float required_rise) {
   // we see the shape of a peak (bottom or top) in this track's window

   //if (rereading) dlog("trk %d peak %d at %.8lf tick %.1lf\n", t->trknum, top, timenow, TICK(timenow));
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
         if (t->datablock || nrzi.datablock) {
            //if (time_adjustment != 0)
            //   dlogtrk("trk %d peak adjust by %4.1f, leftdst %d, rise %.3fV, AGC %.2f, left %.3fV, peak %.3fV, right %.3fV\n",
            //        t->trknum, time_adjustment, left_distance, required_rise, t->agc_gain, t->pkww_v[prevndx], val, t->pkww_v[nextndx]);
            //dlogtrk("trk %d peak of %.3fV at %.8lf tick %.1lf found at %.8lf tick %.1lf, AGC %.2f\n",
            //     t->trknum, val, time, TICK(time), timenow, TICK(timenow), t->agc_gain);
            //show_window(t);//
         }
         t->pkww_countdown = left_distance;  // how long to be blind until the peak exits the window
         //process_transition(t); //old bug???
         return time; }
      ++left_distance;
      if (ndx == t->pkww_right) break;
      prevndx = ndx;
      if (++ndx >= pkww_width) ndx = 0; }
   fatal( "Can't find max or min %f in trk %d window at time %.8lf", val, t->trknum, timenow);
   return 0; }

void lookfor_peak(struct trkstate_t *t) {
   // incorporate this new datum as the right edge of the moving window, discard the value
   // at the left edge, and efficiently keep track of the min and max values within the window
   float old_left = 0;
   if (++t->pkww_right >= pkww_width) t->pkww_right = 0;  // make room for an entry on the right
   if (t->pkww_right == t->pkww_left) {  // if we bump into the left datum (ie, window has filled)
      old_left = t->pkww_v[t->pkww_left]; // then save the old left value
      if (++t->pkww_left >= pkww_width) t->pkww_left = 0; // and  delete it
   }
   t->pkww_v[t->pkww_right] = t->v_now;  // add the new datum on the right
   //if (t->datablock) dlogtrk("at tick %.1lf adding %.2fV, left %.2fV, right %.2fV, min %.2fV, max %.2fV\n",
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
   //if (t->trknum == TRACETRK && timenow >= 0.8609906 && timenow <= 0.8609927) show_window(t);

   if (t->pkww_countdown) {  // if we're waiting for a previous peak to exit the window
      --t->pkww_countdown; } // don't look a the shape within the window yet

   else { // see if the window has the profile of a new top or bottom peak
      assert(t->agc_gain > 0, "AGC gain bad in lookfor_peak: %.2f", t->agc_gain);
      // the following calculation uses t->v_avg_height to set the longterm expectation of the signal magnitude for this track,
      // and t->agc_gain to track shortterm variations. We do the same for the min_peak test.
      float required_rise = PARM.pkww_rise * (t->v_avg_height / (float)PKWW_PEAKHEIGHT) / t->agc_gain;  // how much of a voltage rise constitutes a peak
      float required_min = PARM.min_peak * (t->v_avg_height / (float)PKWW_PEAKHEIGHT) / t->agc_gain; // the minimum peak for this track
      if (0) dlogtrk("trk %d at %.8lf tick %.1f req rise %.3f, avg height %.2f, AGC %.2f, left %.3fV right %.3fV max %.3fV min %.3fV\n",
                        t->trknum, timenow, TICK(timenow), required_rise, t->v_avg_height, t->agc_gain,
                        t->pkww_v[t->pkww_left], t->pkww_v[t->pkww_right], t->pkww_maxv, t->pkww_minv);
      if (t->pkww_maxv > t->pkww_v[t->pkww_left] + required_rise
            && t->pkww_maxv > t->pkww_v[t->pkww_right] + required_rise  // the max is a lot higher than the left and right sides
            && (required_min == 0 || t->pkww_maxv > required_min)) {  // and it's higher than the min peak, if given
         t->v_top = t->pkww_maxv;  // which means we hit a top peak
         t->t_top = refine_peak(t, t->pkww_maxv, true, required_rise);
         dlogtrk("trk %d top of %.3fV, left rise %.3f, right rise %.3f, req rise %.3f, req min %.3f, avg ht %.3f, AGC %.2f at %.8lf tick %.1f, found %.8lf tick %.1f\n",
                 t->trknum, t->v_top,
                 t->pkww_maxv - t->pkww_v[t->pkww_left], t->pkww_maxv - t->pkww_v[t->pkww_right], required_rise, required_min,
                 t->v_avg_height, t->agc_gain, TIMETICK(t->t_top), TIMETICK(timenow));
         process_up_transition(t); }

      else if (t->pkww_minv < t->pkww_v[t->pkww_left] - required_rise
               && t->pkww_minv < t->pkww_v[t->pkww_right] - required_rise  // the min is a lot lower than the left and right sides
               && (required_min == 0 || t->pkww_minv < -required_min)) { // and it's lower than the min peak, if given
         t->v_bot = t->pkww_minv;  //so we hit a bottom peak
         t->t_bot = refine_peak(t, t->pkww_minv, false, required_rise);
         dlogtrk("trk %d bot of %.3fV, left rise %.3f, right rise %.3f, req rise %.3f, req min %.3f, avg ht %.3f, AGC %.2f at %.8lf tick %.1f, found %.8lf tick %.1f\n",
                 t->trknum, t->v_bot,
                 t->pkww_v[t->pkww_left] - t->pkww_minv, t->pkww_v[t->pkww_right] - t->pkww_minv, required_rise, required_min,
                 t->v_avg_height, t->agc_gain, TIMETICK(t->t_bot), TIMETICK(timenow));
         process_down_transition(t); } } }


//-----------------------------------------------------------------------------
//    Process one analog voltage sample with data for all tracks.
//    Return with the updated status of the block we're working on.
//-----------------------------------------------------------------------------
enum bstate_t process_sample(struct sample_t *sample) {

#if DESKEW
   for (int trknum = 0; trknum < ntrks; ++trknum) { // preprocess all tracks to do deskewing
      struct trkstate_t *t = &trkstate[trknum];
      if (skew_delaycnt[trknum] == 0) t->v_now = sample->voltage[trknum]; // no skew delay for this track
      else {
         struct skew_t *skewp = &skew[trknum];
         if (skewp->slots_filled < skew_delaycnt[trknum]) { // haven't built up the history yet
            t->v_now = sample->voltage[trknum]; // keep using the current voltage until we do
            ++skewp->slots_filled; }
         else t->v_now = skewp->vdelayed[skewp->ndx_next]; // use the oldest voltage
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

   if (interblock_counter)  // if we're waiting to get fully into an IBG
      goto exit;            // just exit

   if (nrzi.datablock && timenow > nrzi.t_lastclock + /*(1 + PARM.midbit)*/ 2 * nrzi.clkavg.t_bitspaceavg)
      nrzi_zerocheck();  // do NRZI check for zeros two bit times after the last check

   for (int trknum = 0; trknum < ntrks; ++trknum) {  // look at the analog signal on each track
      struct trkstate_t *t = &trkstate[trknum];
#if !DESKEW
      t->v_now = sample->voltage[trknum]; // no deskewing was done in preprocessing
#endif
      //if (trace_on && mode == PE && timenow - t->t_lastbit > t->t_clkwindow)
      //   TRACE(clkwin, timenow, DNTICK, t); // stop the clock window

      if (t->t_lastpeak == 0) {  // if this is the first sample for this block
         t->pkww_v[0] = t->v_now;  // initialize moving window
         t->pkww_maxv = t->pkww_minv = t->v_now;  // (left/right is already zero)
         t->v_lastpeak = t->v_now;  // initialize last known peak
         t->t_lastpeak = timenow;
         //if (t->trknum == TRACETRK) show_window(t);
         break; }

      if (find_zeros) {
         if (do_differentiate) lookfor_differentiated_zerocrossing(t);
         else lookfor_zerocrossing(t); }
      else lookfor_peak(t);

      if (mode == PE && !t->idle && t->t_lastpeak != 0 && timenow - t->t_lastpeak > t->clkavg.t_bitspaceavg * PE_IDLE_FACTOR) {
         // We waited too long for a PE peak: declare that this track has become idle.
         // (NRZI track data, on the other hand, is allowed to be idle indefinitely.)
         t->v_lastpeak = t->v_now;
         TRACE(clkdet, timenow, DNTICK, t)
         dlogtrk("trk %d became idle at %.8lf, %d idle, AGC %.2f, last peak at %.8lf, bitspaceavg %.2f usec, datacount %d\n", //
                 trknum, timenow, num_trks_idle + 1, t->agc_gain, t->t_lastpeak, t->clkavg.t_bitspaceavg*1e6, t->datacount);
         t->idle = true;
         if (++num_trks_idle >= ntrks) {
            pe_end_of_block(); } }

      if (mode == GCR && t->datablock
            && timenow > t->t_lastpeak + GCR_IDLE_THRESH * t->clkavg.t_bitspaceavg) { // if no peaks for too long
         t->datablock = false; // then we're at the end of the block for this track
         t->idle = true;
         dlog("trk %d becomes idle, %d idle at %.8lf tick %.1lf, AGC %.2f, v_now %f, t_lastpeak %.8lf v_lastpeak %f, bitspaceavg %.2f\n", //
              t->trknum, num_trks_idle + 1, timenow, TICK(timenow), t->agc_gain, t->v_now, t->t_lastpeak, t->v_lastpeak, t->clkavg.t_bitspaceavg*1e6);
         //show_track_datacounts("at idle");
         if (++num_trks_idle >= ntrks) { // and maybe for all tracks
            gcr_end_of_block();
            goto exit; } }

   } // for tracks

   if (mode == WW && ww.datablock
         && ww.t_lastclkpulseend > 0 && timenow - ww.t_lastclkpulseend > ww.clkavg.t_bitspaceavg * WW_CLKSTOP_BITS) {  // the clock has stopped
      ww_end_of_block(); }

#if TRACEFILE
   trace_startstop();
#endif // TRACEFILE

exit:
   if (interblock_counter) {
      if (--interblock_counter) return BS_NONE; // still waiting to really get into an IBG: do nothing
   }
   return block.results[block.parmset].blktype; // done: return the kind of block we got
}
//*
