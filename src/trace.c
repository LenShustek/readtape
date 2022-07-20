//file: trace.c
/*****************************************************************************

   debuging trace file routines

----> See readtape.c for the merged change log.

This stuff creates a CSV trace file with one or all tracks of voltage data
plus all sorts of debugging event info. To see the visual timeline, create a
line graph in Excel from column C through the next blank column.

The compiler switch that turns this on, and the track number to record special
infomation about, is at the top of decoder.h.
The start and end of the graph is controlled by code at the bottom of this file.

The trace data is buffered before being written to the file so that we can
"rewrite history" for events that are discovered late. That happens, for
example, because the moving-window algorithm for peak detection finds
the peaks several clock ticks after they actually happened.

*******************************************************************************
Copyright (C) 2018, 2019, Len Shustek

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

bool trace_on = false, trace_done = false, trace_start = false;
int trace_lines = 0;
FILE *tracef;

#define TRACE_DEPTH 500

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
   { "zerchk", NRZI,     T_ONLYONE,    TB + 5*UPTICK },
   { "parerr", NRZI,     T_ONLYONE,    TB + 7*UPTICK },
   { "clkedg", PE,       0,            TB + 3*UPTICK },
   { "datedg", PE,       0,            TB + 6*UPTICK },
   { "clkwin", PE,       T_PERSISTENT, TB + 9*UPTICK },
   { "clkdet", PE,       T_PERSISTENT, TB + 12*UPTICK },
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
   float t_peakdelta[TRACE_DEPTH];
   uint16_t data[TRACE_DEPTH]; //
}
traceblk = {0 };

void trace_dump(void) { // debugging display the entire trace buffer
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
      int trks_done = 0;
      for (int trk = 0; trk < ntrks; ++trk) { // for all the tracks we're doing
         if (TRACEALL || trk == TRACETRK) {
            float level = TT - trks_done++*TS; // base y-axis level for this track
            fprintf(tracef, "%.4f,",  // output voltage
                    traceblk.voltages[ndx][trk] * TRACESCALE + level);
            for (int i = 0; tracevals[i].name != NULLP; ++i) // do other associated columns
               if ((tracevals[i].mode & mode) && tracevals[i].flags & T_SHOWTRK)
                  fprintf(tracef, "%.2f, ", tracevals[i].val[ndx][trk] + level); } }
      for (int i = 0; tracevals[i].name != NULLP; ++i) { // do the events not shown with the tracks
         if (tracevals[i].mode & mode) { // if we are the right mode
            if (!(tracevals[i].flags & T_PERSISTENT) ||
                  tracevals[i].val[ndx][TRACETRK] != tracevals[i].graphbase)
               tracevals[i].lastval = tracevals[i].val[ndx][TRACETRK];
            if ( !(tracevals[i].flags & T_SHOWTRK)) fprintf(tracef, "%f, ", tracevals[i].lastval); } }
      fprintf(tracef, ", %d, %.2f, %.2f, %.2f, '%03X\n", // do the extra-credit stuff
              traceblk.datacount[ndx], traceblk.agc_gain[ndx], traceblk.bitspaceavg[ndx]*1e6, traceblk.t_peakdelta[ndx]*1e6, traceblk.data[ndx]); } }

void trace_newtime(double time, float deltat, struct sample_t *sample, struct trkstate_t *t) {
   // Create a new timestamped entry in the trace history buffer.
   // It's a circular list, and we write out the oldest entry to make room for the new one.
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
      traceblk.bitspaceavg[traceblk.ndx_next] = mode ==NRZI ? nrzi.clkavg.t_bitspaceavg : t->clkavg.t_bitspaceavg;
      traceblk.t_peakdelta[traceblk.ndx_next] = t->t_peakdelta;
      traceblk.data[traceblk.ndx_next] = data[t->datacount];
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
            fprintf(tracef, "T%d:volts,", trk);
            for (int i = 0; tracevals[i].name != NULLP; ++i) // any associated columns?
               if ((tracevals[i].mode & mode) && tracevals[i].flags & T_SHOWTRK)
                  fprintf(tracef, "T%d:%s, ", trk, tracevals[i].name); // name them
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
      if (mode == NRZI) fprintf(tracef, "bitspaceavg,");
      else fprintf(tracef, "T%d bitspaceavg,", TRACETRK);
      fprintf(tracef, "T%d peak deltaT, data\n", TRACETRK); } }

void trace_close(void) {
   if (trace_on) {
      trace_on = false;
      trace_done = true;
      dlog("-----> trace stopped with %d lines at %.8lf tick %.1lf\n", trace_lines, timenow, TICK(timenow)); }
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
      assert(time <= timenow, "trace event \"%s\" at %.8lf too new at %.8lf", tracevals[tracenum].name, time, timenow);
      bool event_found = time > traceblk.time_newest - TRACE_DEPTH * traceblk.deltat;
      if (!event_found) trace_dump();
      assert(event_found, "trace event \"%s\" at %.8lf too old, at %.8lf", tracevals[tracenum].name, time, timenow);
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
      //   rlog("clkwin %.2f at %.8lf, tick %.1lf\n", tickdirection, time, TICK(time));//
   } };



void trace_startstop(void) {

//**** Choose a test here for turning the trace on, depending on what anomaly we're looking at...

   //if (rereading
   //if (true
   //if (ww.datablock && numblks >= 2
   if (timenow > 1.93907
         //if (numblks >= 1478 && block.parmset == 2 && timenow > 163.863
         //if (timenow > 13.1369 && trkstate[0].datacount > 270
         //if (trkstate[TRACETRK].peakcount > 0
         //if (trkstate[TRACETRK].datacount < trkstate[0].datacount-3
         //if (trkstate[TRACETRK].v_now < -2.0
         //if (trkstate[4].datacount > 0
         //if (trkstate[5].v_now > 0.5
         //if (nrzi.clkavg.t_bitspaceavg > 40e-6
         //if (nrzi.datablock
         //if (!trkstate[TRACETRK].idle
         //if (num_trks_idle < ntrks
         //if (trace_start
         //
         && !doing_deskew && !doing_density_detection && !trace_on && !trace_done) {
      trace_open();
      trace_on = true;
      torigin = timenow - sample_deltat;
      dlog("-----> trace started at %.8lf tick %.1lf, block %d parmset %d\n",
           timenow, TICK(timenow), numblks+1, block.parmset); }
   if (trace_on && ++trace_lines > 10000) { //**** limit on how much trace data to collect
      trace_close(); } }


//*
