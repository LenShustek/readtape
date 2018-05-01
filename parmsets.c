// file: parmsets.c
/**********************************************************************

routines for reading and parsing parameter sets,
and for setting their default values

The format of the <basefilename>.parms (or NRZI.parms or PE.parms) file is:
  //  comments
  readtape <additional command line options>
  parms active, clk_window,  clk_alpha, agc_window, agc_alpha, min_peak, clk_factor, pulse_adj, pkww_bitfrac, id
  {1,   0,     0.2,      5,    0.0,    0.0,   1.50,    0.4,   0.7, "PRM" },
  ...

The "parms" line of names defines the order in which the parameter values are provided in the file.
If there is a now-obsolete parm name that the program no longer knows about, it is ignored with a warning.
If a parm name that the program expects is missing, the value from the first default parmset is used.
That scheme allows us to add and remove parm types in the program without invalidating existing .parm files.

Adding a new parameter requires:
 1. adding a field to struct blkstate_t in decoder.h
 2. adding a line to "parms" in this file, giving the type, name, and min/max values
 3. adding default values to all the struct parms_t parmsets_xxx initialization in this file
 4. perhaps adding a display at the end of readtape.c
 5. referencing the parameter as PARM.xxxx, where appropriate
 
---> See readtape.c for the merged change log <----

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

#include "decoder.h"

struct parmdescr_t {    // define the currently known parameters in a parameter set
   enum { P_INT, P_FLT, P_STR, P_END } type;
   char *name;
   enum mode_t mode;
   float min, max;
   int offset; // offset in the parms_t structure,
#define DEFINE_PARM(t,n,m,min,max) {t,#n,m,min,max,offsetof(struct parms_t,n)}
   // That funny business lets us assign to variables whose name is given at runtime.
   // This would be a reason to use Python instead of C, except it's *much* too slow!
}
parms[] = { // list of: type, name, min value, max value
   // can reorder between 'active" and "id", as long as the order agrees with the defaults below
   DEFINE_PARM(P_INT, active, ALL, 0.0, 1.0),
   DEFINE_PARM(P_INT, clk_window, ALL, 0.0, CLKRATE_WINDOW),
   DEFINE_PARM(P_FLT, clk_alpha, ALL, 0.0, 1.0),
   DEFINE_PARM(P_INT, agc_window, ALL, 0.0, AGC_MAX_WINDOW),
   DEFINE_PARM(P_FLT, agc_alpha, ALL, 0.0, 1.0),
   DEFINE_PARM(P_FLT, min_peak, ALL, 0.0, 5.0),
   DEFINE_PARM(P_FLT, clk_factor, PE, 0.0, 2.0),
   DEFINE_PARM(P_FLT, pulse_adj, ALL, 0.0, 1.0),
   DEFINE_PARM(P_FLT, pkww_bitfrac, ALL, 0.0, 1.0),
   DEFINE_PARM(P_STR, id, ALL, 0, 0),
   {P_END } };

struct parms_t parmsets_PE[MAXPARMSETS] = {  //*** default parmsets for 1600 BPI PE ***
   // clkwin  clkalpha  agcwin agcalpha minpk  clkfact  pulseadj bitfrac
   { 1,   0,     0.2,      5,    0.0,    0.0,   1.50,    0.4,   0.7, "PRM" },
   { 1,   3,     0.0,      5,    0.0,    0.0,   1.40,    0.0,   0.7, "PRM" }, // works on block 55, but not with pulseadj=0.2
   { 1,   3,     0.0,      5,    0.0,    0.0,   1.40,    0.2,   0.7, "PRM" },
   { 1,   5,     0.0,      5,    0.0,    0.0,   1.40,    0.0,   0.7, "PRM" },
   { 1,   5,     0.0,      5,    0.0,    0.0,   1.50,    0.2,   0.7, "PRM" },
   { 1,   5,     0.0,      5,    0.0,    0.0,   1.40,    0.4,   0.7, "PRM" },
   { 1,   3,     0.0,      5,    0.0,    0.0,   1.40,    0.2,   0.7, "PRM" },
   { 0 } };

struct parms_t parmsets_NRZI[MAXPARMSETS] = { //*** default parmsets for 800 BPI NRZI ***
   // clkwin  clkalpha  agcwin agcalpha  minpk  clkfact  pulseadj bitfrac
   { 1,   0,     0.2,      0,     0.3,     1.3,   0,     0.5,   0.7, "PRM" },
   { 1,   0,     0.3,      0,     0.3,     1.3,   0,     0.5,   0.7, "PRM" },
   { 1,   2,     0.0,      0,     0.3,     1.3,   0,     0.5,   0.7, "PRM" },
   { 1,   0,     0.6,      0,     0.3,     1.3,   0,     0.5,   0.8, "PRM" },
   { 0 } };

struct parms_t parmsets_GCR[MAXPARMSETS] = { //*** default parmsets for 6250 BPI GCR ***
   // clkwin  clkalpha  agcwin agcalpha  minpk  clkfact  pulseadj bitfrac
   { 1,   0,     0.4,      5,     0.0,     0.50,   0,     0.2,   0.8, "PRM" },
   { 0 } };


struct parms_t parmsets[MAXPARMSETS] = { 0 };
struct parms_t *parmsetsptr = parmsets;  // pointer to parmsets array we're using
char line[MAXLINE];
#define MAXSTRING 100

void skip_blanks(char **pptr) {
   while (**pptr == ' ' || **pptr == '\t')++*pptr; }

bool scan_key(char **pptr, const char *keyword) {
   skip_blanks(pptr);
   char *ptr = *pptr;
   do if (tolower(*ptr++) != *keyword++) return false;
   while (*keyword);
   *pptr = ptr;
   skip_blanks(pptr);
   return true; }

bool scan_float(char **pptr, float *pnum, float min, float max) {
   float num;  int nch;
   if (sscanf(*pptr, "%f%n", &num, &nch) != 1
         || num < min || num > max ) return false;
   *pnum = num;
   *pptr += nch;
   skip_blanks(pptr);
   return true; }

bool scan_int(char **pptr, int *pnum, int min, int max) {
   int num;  int nch;
   if (sscanf(*pptr, "%d%n", &num, &nch) != 1
         || num < min || num > max ) return false;
   *pnum = num;
   *pptr += nch;
   skip_blanks(pptr);
   return true; }

bool scan_str(char **pptr, char *str) { // scan an alphnumeric (plus _) string
   skip_blanks(pptr);
   if (iscntrl(**pptr)) return false;
   for (int nch = 0; nch < MAXSTRING-1 && (isalnum(**pptr) || **pptr == '_'); ++nch, ++*pptr) *str++ = **pptr;
   *str = '\0';
   skip_blanks(pptr);
   return true; }

bool scan_to_blank(char **pptr, char *str) { // scan to a blank or newline
   skip_blanks(pptr);
   for (int nch = 0; nch < MAXSTRING - 1 && **pptr != ' ' && !iscntrl(**pptr); ++nch, ++*pptr) *str++ = **pptr;
   *str = '\0';
   skip_blanks(pptr);
   return true; }

void dump_parms(bool showall) {
   rlog("  ");
   for (int i = 0; i < MAXPARMSETS && parms[i].type != P_END; ++i)
      if (showall || parms[i].mode == ALL || parms[i].mode == mode)
         rlog(parms[i].type == P_STR ? "%4s\n" : "%10.10s, ", parms[i].name);
   for (struct parms_t *setptr = parmsets; setptr->active == 1; ++setptr) {
      rlog("{");
      for (int i = 0; i < MAXPARMSETS && parms[i].type != P_END; ++i) {
         if (showall || parms[i].mode == ALL || parms[i].mode == mode)
            switch (parms[i].type) {
            case P_INT: rlog("%10d, ", *(int *)((char*)setptr + parms[i].offset)); break;
            case P_FLT: rlog("%10.3f, ", *(float *)((char*)setptr + parms[i].offset)); break;
            case P_STR: rlog("%6s },\n", (char *)setptr + parms[i].offset); break; //
            } } } }

struct parms_t *default_parmset(void) {
   if (mode == PE) return parmsets_PE;
   else if (mode == NRZI) return parmsets_NRZI;
   else if (mode == GCR) return parmsets_GCR;
   fatal("bad mode"); return NULLP; }

void read_parms(void) {   // process the optional <basefilename>.parms file of parameter sets for the current data
   FILE *parmf;
   char filename[MAXPATH];
   struct parms_t *setptr;

   strncpy(filename, basefilename, MAXPATH - 7);  // try filename.parms
   filename[MAXPATH - 7] = '\0';
   strcat(filename, ".parms");
   parmf = fopen(filename, "r");
   if (parmf == NULLP) {
      strcpy(filename, modename());  // try NRZI.parms, PE.parms, etc.
      strcat(filename, ".parms");
      parmf = fopen(filename, "r");
      if (parmf == NULLP) {
         // no parameter sets file: use the default set for this type of tape
         setptr = default_parmset();
         memcpy(parmsets, setptr, sizeof(parmsets));
         if (!quiet) {
            rlog(".parms file not found; using defaults for parameter sets\n");
            dump_parms(false); }
         return; } }

   if (!quiet) rlog("reading parmsets from \"%s\"\n", filename);

   bool got_parmnames = false;
   int numsets = 0;
   int file_to_parm[MAXPARMS];         // translate from file parm order to our parm order
   bool parm_given[MAXPARMS];          // was this parm of ours given?
   int numfileparms = 0;
   for (int i = 0; i < MAXPARMS; ++i) {
      file_to_parm[i] = -1; // "not mapped to any parm we know"
      parm_given[i] = false; }

   setptr = parmsets;
   while (fgets(line, MAXLINE, parmf)) {
      char *ptr = line;
      char str[MAXSTRING];
      if (scan_key(&ptr, "//")); // ignore comment line
      else if (*ptr == '\n' || *ptr == '\0'); // ignore blanks line

      else if (scan_key(&ptr, "readtape")) { // process additions to command-line options
         while (*ptr != '\n') {
            skip_blanks(&ptr);
            scan_to_blank(&ptr, str);
            assert(parse_option(str), "bad option in %s: %s", filename, str); } }

      else if (scan_key(&ptr, "parms")) { // process parameter names
         for (int filendx = 0; ; ++filendx) { // scan file parm names
            assert(scan_str(&ptr, str), "missing parm name at %s", ptr);
            assert(++numfileparms < MAXPARMS, "too many parm names at %s", ptr);
            for (int i = 0; i < MAXPARMS; ++i) { // search for a match in our table
               if (parms[i].type == P_END) { // no match
                  rlog("  --->obsolete parm ignored: %s\n", str);
                  break; }
               if (strcmp(parms[i].name, str) == 0) { // match
                  file_to_parm[filendx] = i;
                  parm_given[i] = true;
                  break; } }
            if (!scan_key(&ptr, ",")) break; }
         assert(*ptr == '\n', "bad parm name: %s", ptr);
         got_parmnames = true; }

      else if (scan_key(&ptr, "{")) { // process a list of parameter values, in the order of the file's names
         assert(got_parmnames, "missing parameter names line");
         for (int filendx = 0; filendx < numfileparms; ++filendx) {
            float fval;
            int ourndx = file_to_parm[filendx];
            if (ourndx == -1)
               assert(scan_float(&ptr, &fval, 0, 99), "bad parm at %s", ptr); // skip obsolete parm value
            else {
               if (parms[ourndx].type == P_FLT) {
                  assert(scan_float(&ptr, &fval, parms[ourndx].min, parms[ourndx].max), "bad parm at %s", ptr);
                  *(float*)((char*)setptr + parms[ourndx].offset) = fval; }
               else if (parms[ourndx].type == P_INT) {
                  assert(scan_float(&ptr, &fval, parms[ourndx].min, parms[ourndx].max), "bad parm at %s", ptr);
                  *(int*)((char*)setptr + parms[ourndx].offset) = (int)fval; } }
            scan_key(&ptr, ","); }
         assert(scan_key(&ptr, "\"prm\""), "missing \"PRM\" at %s", ptr);
         strcpy(setptr->id, "PRM");
         assert(scan_key(&ptr, "}"), "missing parmset closing }");
         ++setptr;  // next slot to fill
      }
      else fatal("bad parmset file input: \"%s\"", ptr);
      assert(++numsets < MAXPARMSETS, "too many parmsets at %s", ptr); }

// look for new parms of ours that weren't given in the file
   for (int ourparmndx=0; ourparmndx<MAXPARMS && parms[ourparmndx].type != P_END; ++ourparmndx)
      if (!parm_given[ourparmndx]) {
         setptr = default_parmset(); // prepare to use the value from the first default set of parms
         for (int setndx = 0; setndx < MAXPARMSETS && parmsets[setndx].active == 1; ++setndx) { // for all sets
            if (parms[ourparmndx].type == P_FLT) {
               float defaultval = *(float *)((char*)setptr + parms[ourparmndx].offset);
               *(float*)((char*)&parmsets[setndx] + parms[ourparmndx].offset) = defaultval ;
               if (setndx == 0) rlog("  --->missing parm %s; using default of %.3f for all parmsets\n", parms[ourparmndx].name, defaultval); }
            else if (parms[ourparmndx].type == P_INT) {
               int defaultval = *(int *)((char*)setptr + parms[ourparmndx].offset);
               *(int*)((char*)&parmsets[setndx] + parms[ourparmndx].offset) = defaultval;
               if (setndx == 0) rlog("  --->missing parm %s; using default of %d for all parmsets\n", parms[ourparmndx].name, defaultval); } } }

   if (verbose) dump_parms(false); // show the parms as read from the file
}

//*