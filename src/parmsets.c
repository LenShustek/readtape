// file: parmsets.c
/*********************************************************************************************************************

routines for reading and parsing parameter sets,
and for setting their default values

The format of the <basefilename>.parms (or NRZI.parms, PE.parms, or GCR.parms) file is:
   //  comments
   readtape <additional command line options>
   parms active, clk_window,  clk_alpha, agc_window, agc_alpha, min_peak, clk_factor, pulse_adj, pkww_bitfrac, id
   {1,   0,     0.2,      5,    0.0,    0.0,   1.50,    0.4,   0.7, "PRM" },
   ...

The "parms" line of names defines the order in which the parameter values are provided in the file.
If there is a now-obsolete parm name that the program no longer knows about, it is ignored with a warning.
If a parm name that the program expects is missing, the value from the first default parmset is used.
That scheme allows us to add and remove parm types in the program without invalidating existing .parm files.
Only parms needed for the particular mode (NRZI, PE, or GCR) need be given; others are ignored with a warning.

Adding a new parameter requires:
 1. adding a field to struct parms_t in decoder.h
 2. adding a line to "parms" in this file, giving the type, name, and min/max values
 3. adding the parm name and values, where appropriate, to the parmcmds_xxxx[] command strings in this file
 4. referencing the parameter as PARM.xxxx in the code, where appropriate

---> See readtape.c for the merged change log <----

*********************************************************************************************************************
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
parms[] = { // list of: type, name, min_value, max_value
   // can reorder between 'active" and "id", but the order must agree with the defaults below
   DEFINE_PARM(P_INT, active, ALLMODES, 0.0, 1.0),
   DEFINE_PARM(P_INT, clk_window, ALLMODES, 0.0, CLKRATE_WINDOW),
   DEFINE_PARM(P_FLT, clk_alpha, ALLMODES, 0.0, 1.0),
   DEFINE_PARM(P_INT, agc_window, ALLMODES, 0.0, AGC_MAX_WINDOW),
   DEFINE_PARM(P_FLT, agc_alpha, ALLMODES, 0.0, 1.0),
   DEFINE_PARM(P_FLT, min_peak, ALLMODES, 0.0, 5.0),
   DEFINE_PARM(P_FLT, clk_factor, PE, 0.0, 2.0),
   DEFINE_PARM(P_FLT, pulse_adj, ALLMODES-WW, 0.0, 1.0),
   DEFINE_PARM(P_FLT, pkww_bitfrac, ALLMODES, 0.0, 2.0),
   DEFINE_PARM(P_FLT, pkww_rise, ALLMODES, 0.0, 5.0),
   DEFINE_PARM(P_FLT, midbit, NRZI, 0.0, 1.0),
   DEFINE_PARM(P_FLT, z1pt, GCR, 1.0, 2.0),
   DEFINE_PARM(P_FLT, z2pt, GCR, 2.0, 3.0),
   DEFINE_PARM(P_STR, id, ALLMODES, 0, 0),
   {P_END } };

struct parms_t parmsets_PE[MAXPARMSETS] = {  0 }; // where we store the PE default parmsets
char *parmcmds_PE[MAXPARMSETS] = { // commands to set defaults for PE
   "parms active, clk_window, clk_alpha, agc_window, agc_alpha, min_peak, clk_factor, pulse_adj, pkww_bitfrac, pkww_rise, id",
   "{       1,       0,         0.2,            5,     0.0,       0.0,      1.50,       0.4,          0.7,       0.10,  PRM }",
   "{       1,       0,         0.2,            5,     0.0,       0.1,      1.50,       0.4,          0.7,       0.10,  PRM }",
   "{       1,       3,         0.0,            5,     0.0,       0.0,      1.40,       0.0,          0.7,       0.10,  PRM }", // works on block 5, but not with pulseadj=0.2
   "{       1,       3,         0.0,            5,     0.0,       0.0,      1.40,       0.2,          0.7,       0.10,  PRM }",
   "{       1,       5,         0.0,            5,     0.0,       0.0,      1.40,       0.0,          0.7,       0.10,  PRM }",
   "{       1,       5,         0.0,            5,     0.0,       0.0,      1.50,       0.2,          0.7,       0.10,  PRM }",
   "{       1,       5,         0.0,            5,     0.0,       0.0,      1.40,       0.4,          0.7,       0.10,  PRM }",
   "{       1,       3,         0.0,            5,     0.0,       0.0,      1.40,       0.2,          0.7,       0.10,  PRM }",
   {0 } };

struct parms_t parmsets_NRZI[MAXPARMSETS] = { 0 }; // where we store the NRZI default parmsets
char *parmcmds_NRZI[MAXPARMSETS] = { // commands to set defaults for NRZI
   "parms  active, clk_window, clk_alpha, agc_window, agc_alpha, min_peak, pulse_adj, pkww_bitfrac, pkww_rise, midbit,  id",
   "{        1,       0,      0.200,          0,      0.300,      1.000,      0.300,      0.700,      0.200,      0.500,   PRM }",
   "{        1,       0,      0.300,          0,      0.300,      1.000,      0.400,      0.600,      0.200,      0.500,   PRM }",
   "{        1,       2,      0.000,          0,      0.300,      1.000,      0.400,      0.700,      0.200,      0.500,   PRM }",
   "{        1,       0,      0.600,          0,      0.300,      1.000,      0.400,      0.600,      0.200,      0.500,   PRM }",
   "{        1,       2,      0.000,          1,      0.000,      0.500,      0.500,      0.900,      0.050,      0.500,   PRM }", // for shallow peaks
   "{        1,       0,      0.200,          1,      0.000,      1.000,      0.500,      0.700,      0.050,      0.500,   PRM }",
   "{        1,       2,      0.000,          1,      0.000,      0.500,      0.500,      0.700,      0.050,      0.500,   PRM }",
   "{        1,       0,      0.600,          1,      0.000,      0.500,      0.500,      0.600,      0.050,      0.500,   PRM }",
   { 0 } };

struct parms_t parmsets_GCR[MAXPARMSETS] = { 0 }; // where we store the GCR default parmsets
char *parmcmds_GCR[MAXPARMSETS] = { // commands to set defaults for GCR
   "parms  active, clk_window, clk_alpha, agc_window, agc_alpha, min_peak, pulse_adj, pkww_bitfrac, pkww_rise, z1pt, z2pt, id",
   "{         1,          0,      0.015,       0,      0.500,      0.200,   0.300,      1.500,      0.200,     1.450,  2.350,   PRM }",
   "{         1,          0,      0.020,       0,      0.500,      0.200,   0.300,      1.500,      0.200,     1.450,  2.350,   PRM }",
   "{         1,          0,      0.010,       0,      0.500,      0.200,   0.300,      1.500,      0.200,     1.450,  2.350,   PRM }",
   "{         1,         10,      0.000,       0,      0.500,      0.000,   0.600,      1.500,      0.140,     1.400,  2.300,   PRM }",
   "{         1           0       0.020,       0,      0.500,      0.200,   0.300,      1.500,      0.200,     1.480,  2.350,   PRM }",
   { 0 } };

struct parms_t parmsets_WW[MAXPARMSETS] = { 0 }; // where we store the Whirlwind default parmsets
char *parmcmds_WW[MAXPARMSETS] = { // commands to set defaults for GCR
   "parms  active, clk_window, clk_alpha, agc_window, agc_alpha, min_peak, pkww_bitfrac, pkww_rise, id",
   "{         1,          0,      0.050,       0,      0.500,      1.000,    0.400,      0.200,    PRM }",
   "{         1,          0,      0.020,       0,      0.500,      0.050,    0.200,      0.200,    PRM }",
   { 0 } };


struct parms_t parmsets[MAXPARMSETS] = { 0 }; // the parmsets we construct and then use
struct parms_t *parmsetsptr = parmsets;       // pointer to parmsets array we're using
char line[MAXLINE];

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
   for (int nch = 0; nch < MAXLINE-1 && (isalnum(**pptr) || **pptr == '_'); ++nch, ++*pptr) *str++ = **pptr;
   *str = '\0';
   skip_blanks(pptr);
   return true; }

bool getchars_to_blank(char **pptr, char *dstptr) {
   // Get characters up to a non-quoted blank or newline.
   // We remove "..." from around quoted strings, and allow \" as a literal.
   bool inquote = false;
   char *srcptr = *pptr, lastchar = 0, nextchar;
   int nch = 0;
   while (1) {
      nextchar = *srcptr;
      if (lastchar == '\\' && nextchar == '"') {
         *(dstptr - 1) = '"'; ++srcptr; } // change \"  to "
      else {
         if (inquote && nextchar == '"') {
            inquote = false; ++srcptr; }
         else {
            if (nextchar == '"') {
               inquote = true; ++srcptr; }
            else { // ready to copy a char
               if (iscntrl(nextchar)) {
                  if (inquote) return false; // quoted string wasn't closed
                  goto goodend; }
               if (!inquote && nextchar == ' ') goto goodend;
               *dstptr++ = nextchar; ++srcptr;
               if (++nch >= MAXLINE - 1) return false; } } }
      lastchar = nextchar; }
goodend:
   *dstptr = '\0';
   *pptr = srcptr;      // update caller's pointer to past our characters
   skip_blanks(pptr);   // and then skip following blanks
   return true; }

void show_parms(struct parms_t *psptr, bool showall) {
   rlog("  parms ");
   for (int i = 0; i < MAXPARMSETS && parms[i].type != P_END; ++i)
      if (showall || parms[i].mode & mode)
         rlog(parms[i].type == P_STR ? "%4s\n" : "%11s,", parms[i].name);
   for (struct parms_t *setptr = psptr; setptr->active == 1; ++setptr) {
      rlog("  {   ");
      for (int i = 0; i < MAXPARMSETS && parms[i].type != P_END; ++i) {
         if (showall || parms[i].mode & mode)
            switch (parms[i].type) {
            case P_INT: rlog("%10d, ", *(int *)((char*)setptr + parms[i].offset)); break;
            case P_FLT: rlog("%10.3f, ", *(float *)((char*)setptr + parms[i].offset)); break;
            case P_STR: rlog("  %s}", (char *)setptr + parms[i].offset); break; //
            } }
      if (setptr->comment[0]) rlog(" //%s", setptr->comment);
      else rlog("\n"); }
   rlog("\ncompile-time decoding constants:\n");
   if (find_zeros) {
      rlog("  minimum excursion before considering a zero crossing: %.3fV\n", ZEROCROSS_PEAK);
      rlog("  maximum time in bits for the required excursion to be attained: %.1f bit times\n", ZEROCROSS_SLOPE); }
   else { // peak detection
      rlog("  peak height closeness threshold: %.3fV\n", PEAK_THRESHOLD);
      rlog("  nominal peak height for rise calculation: %.1fV\n", PKWW_PEAKHEIGHT / 2); }
   rlog("  AGC maximum: %.0f\n", (float)AGC_MAX_VALUE);
   if (mode == GCR) {
      rlog("  GCR idle threshold: %.2f bits\n", GCR_IDLE_THRESH); }
   if (mode == PE) {
      rlog("  PE idle treshold: %.2f bits\n", PE_IDLE_FACTOR); }
   if (mode == WW) {
      rlog("  Whirlwind clock stop detect time:   %03.1f bits\n", WW_CLKSTOP_BITS);
      rlog("  Whirlwind peak same-bit  threshold: %03.1f bits\n", WW_PEAKSCLOSE_BITS);
      rlog("  Whirlwind peak unrelated threshold: %03.1f bits\n", WW_PEAKSFAR_BITS);
      rlog("  Whirlwind clock variation warning threshold: %.0f%%\n", WW_MAX_CLK_VARIATION * 100); } }

struct parms_t *default_parmset(void) {
   if (mode == PE) return parmsets_PE;
   else if (mode == NRZI) return parmsets_NRZI;
   else if (mode == GCR) return parmsets_GCR;
   else if (mode == WW) return parmsets_WW;
   fatal("bad mode"); return NULLP; }

// parse parameter-setting commands, either from a file or from precompiled strings
void parse_parms(struct parms_t *parmarray, char*(*getnextline)(void)) {
   bool got_parmnames = false;
   int numsets = 0;
   int file_to_parm[MAXPARMS];         // translate from file parm order to our parm order
   bool parm_given[MAXPARMS];          // was this parm of ours given?
   int numfileparms = 0;
   for (int i = 0; i < MAXPARMS; ++i) {
      file_to_parm[i] = -1; // "not mapped to any parm we know"
      parm_given[i] = false; }
   struct parms_t *setptr = parmarray;
   char *ptr;
   while (ptr = (*getnextline)()) {
      //rlog("parsing line: %s\n", ptr);
      char str[MAXLINE];
      if (scan_key(&ptr, "//")); // ignore comment line
      else if (*ptr == '\n' || *ptr == '\0'); // ignore blanks line

      else if (scan_key(&ptr, "readtape")) { // process additions to command-line options
         rlog("readtape %s", ptr);
         while (*ptr != '\n') {
            skip_blanks(&ptr);
            assert(getchars_to_blank(&ptr, str), "bad option string in parms file: %s", ptr);
            assert(parse_option(str), "bad option from parms file: %s", str); } }

      else if (scan_key(&ptr, "parms")) { // process parameter names
         scan_key(&ptr, ":");
         for (int filendx = 0; ; ++filendx) { // scan file parm names
            assert(scan_str(&ptr, str), "missing %s parm name at %s", modename(), ptr);
            assert(++numfileparms < MAXPARMS, "too many parm names at %s", ptr);
            for (int i = 0; i < MAXPARMS; ++i) { // search for a match in our table
               if (parms[i].type == P_END) { // no match
                  rlog("  --->obsolete %s parm ignored: %s\n", modename(), str);
                  break; }
               if (strcmp(parms[i].name, str) == 0) { // match
                  file_to_parm[filendx] = i;
                  parm_given[i] = true;
                  if (!(parms[i].mode & mode))
                     rlog("  --->parm %s ignored because it isn't used for %s\n",
                          parms[i].name, modename());
                  break; } }
            if (!scan_key(&ptr, ",")) break; }
         assert(*ptr == '\n' || *ptr == 0, "bad parm name: %s", ptr);
         got_parmnames = true; }

      else if (scan_key(&ptr, "{")) { // process a list of parameter values, in the order of the file's names
         assert(got_parmnames, "missing parameter names line");
         for (int filendx = 0; filendx < numfileparms; ++filendx) {
            float fval;
            int ourndx = file_to_parm[filendx];
            if (ourndx == -1)
               assert(scan_float(&ptr, &fval, 0, 99), "bad obsolete parm in parmset %d at: %s", numsets+1, ptr); // skip obsolete parm value
            else {
               if (parms[ourndx].type == P_FLT) {
                  assert(scan_float(&ptr, &fval, parms[ourndx].min, parms[ourndx].max),
                         "bad floating point parm in parmset %d for \"%s\" at: %s",
                         numsets+1, parms[ourndx].name, ptr);
                  *(float*)((char*)setptr + parms[ourndx].offset) = fval; }
               else if (parms[ourndx].type == P_INT) {
                  assert(scan_float(&ptr, &fval, parms[ourndx].min, parms[ourndx].max),
                         "bad integer parm in parmset %d for \"%s\" at: %s",
                         numsets+1, parms[ourndx].name, ptr);
                  // (I forget why we aren't using scan_int() here, but there must have been a reason...)
                  *(int*)((char*)setptr + parms[ourndx].offset) = (int)fval; } }
            scan_key(&ptr, ","); }
         assert(scan_key(&ptr, "\"prm\"") || scan_key(&ptr, "prm"), "missing \"PRM\" in parmset %d at: %s", numsets+1, ptr);
         strcpy(setptr->id, "PRM");
         assert(scan_key(&ptr, "}"), "missing parmset closing } in parmset %d", numsets+1);
         if (scan_key(&ptr, "//")) { // see if there is a comment following
            strncpy(setptr->comment, ptr, MAXPARMCOMMENT); // if so, remember it for printing
            setptr->comment[MAXPARMCOMMENT - 1] = '\0'; }
         ++setptr;  // next slot to fill
         assert(++numsets < MAXPARMSETS, "too many parmsets at: %s", ptr); }
      else fatal("bad parmset file input: \"%s\"", ptr); }
   assert(numsets > 0, "no parameter sets given");

   // look for parms needed for our mode that weren't given in the file
   for (int ourparmndx = 0; ourparmndx<MAXPARMS && parms[ourparmndx].type != P_END; ++ourparmndx)
      if (!parm_given[ourparmndx]) {
         setptr = default_parmset(); // prepare to use the value from the first default set of parms in all the sets we were given
         for (int setndx = 0; setndx < MAXPARMSETS && parmsets[setndx].active == 1; ++setndx) { // for all sets
            if (parms[ourparmndx].type == P_FLT) {
               float defaultval = *(float *)((char*)setptr + parms[ourparmndx].offset);
               *(float*)((char*)&parmsets[setndx] + parms[ourparmndx].offset) = defaultval;
               // complain only once, and only if the mode we're in (PE, NRZI, GCR) uses this parm
               if (setndx == 0 && (parms[ourparmndx].mode & mode))
                  rlog("  --->missing %s floating point parm %s; using default of %.3f for all parmsets\n", modename(), parms[ourparmndx].name, defaultval); }
            else if (parms[ourparmndx].type == P_INT) {
               int defaultval = *(int *)((char*)setptr + parms[ourparmndx].offset);
               *(int*)((char*)&parmsets[setndx] + parms[ourparmndx].offset) = defaultval;
               // complain only once, and only if the mode we're in (PE, NRZI, GCR) uses this parm
               if (setndx == 0 && (parms[ourparmndx].mode & mode))
                  rlog("  --->missing %s integer parm %s; using default of %d for all parmsets\n", modename(), parms[ourparmndx].name, defaultval); } } } };

FILE *parmf;
char *next_file_line(void) { // get the next line from the .parms file
   return fgets(line, MAXLINE, parmf); }

char **precompiled_line_ptr;
char *next_precompiled_line(void) { // get the next line from the precompiled parm commands
   return *precompiled_line_ptr++; }

void read_parms(void) {   // process the optional .parms file of parameter sets for the current data
   char filename[MAXPATH];
   struct parms_t *setptr;

   //rlog("parsing precompiled default parms\n"); // but first: parse the built-in parms for this mode
   precompiled_line_ptr =
      mode == PE ? parmcmds_PE :
      mode == NRZI ? parmcmds_NRZI :
      mode == GCR ? parmcmds_GCR :
      mode == WW ? parmcmds_WW :
      NULLP;
   assert(precompiled_line_ptr != NULLP, "bad mode in read_parms");
   parse_parms(default_parmset(), next_precompiled_line);
   //show_parms(default_parmset(), false);

   strncpy(filename, baseinfilename, MAXPATH - 7);  // try <basefilename>.parms
   filename[MAXPATH - 7] = '\0';
   strcat(filename, ".parms");
   parmf = fopen(filename, "r");
   if (parmf == NULLP) {
      char *filename_slash = strrchr(filename, '\\'); // find specified directory path, if any is before the <basefilename>
      if (filename_slash != NULLP) {
         strcpy(filename_slash + 1, modename());
         strcat(filename, ".parms"); // try NRZI.parms, PE.parm, etc. in that directory
         parmf = fopen(filename, "r"); }
      if (filename_slash == NULLP || parmf == NULLP) {
         strcpy(filename, modename());  // try NRZI.parms, PE.parms, etc. in the current directory
         strcat(filename, ".parms");
         parmf = fopen(filename, "r");
         if (parmf == NULLP) {
            // no parameter sets file: use the default set for this type of tape
            setptr = default_parmset();
            memcpy(parmsets, setptr, sizeof(parmsets));
            if (!quiet) {
               rlog("\nno .parms file was found, so we're using these internal defaults for the %s parameter sets:\n", modename());
               show_parms(parmsets, false); }
            return; } } }
   if (!quiet) rlog("\nreading parmsets from file %s\n", filename);
   parse_parms(parmsets, next_file_line);
   if (!quiet) show_parms(parmsets, false); // show the parms we will be using
}

//*