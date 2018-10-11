//file: readtape.c
/*****************************************************************************

Read the analog signals recorded from the heads of a magnetic tape,
decode the data in any of several formats and speeds,
and create one or more files with the original information.

readtape <options> <baseinfilename>

The input is either:
  - a CSV file with multiple columns:a timestamp in seconds,
    and then the read head voltages at that time for each of the tracks.
  - a TBIN binary compressed data file; see csvtbin.h

See the "A_documentation.txt" file for a narrative about usage,
internal operation, and the algorithms we use.

******************************************************************************
Copyright (C) 2018, Len Shustek

---CHANGE LOG ---

*** 20 Jan 2018, L. Shustek, Started first version

*** 05 Feb 2018, L. Shustek, First github posting, since it kinda works.

*** 09 Feb 2018, L. Shustek
- Replace sscanf for a 20x speedup!
- Made major changes to the decoding algorithm:
 - In the voltage domain, do idle detection based on peak-to-peak
   voltage, not proximity to a baseline resting level.
 - In the time domain, do clock simulation when the signal drops out.
   In other words,,we fake bits when we see no flux transitions.

*** 12 Feb 2018, L. Shustek	 Major restructure:
- Allow blocks to be processed multiple times with different sets of
  deocding parameters. We then choose the best of the bad decodings.

*** 19 Feb 2018, L. Shustek. Change to the peak detection algorithm:
- Interpolate the time of the peak when multiple samples are close to
  the peak. This significantly improves decoding for low sampling rates.
  Sampling at 781 KS/s for 1600 BPI PE tapes running at 50 IPS generates
  about 10 samples per bit, and seems to work fine now.

*** 20 Feb 2018, L. Shustek,
- Add the "move threshold" to the variable parameters, because setting it
  low for some blocks lets us correctly decode some really grungy data.

*** 26 Feb 2018, L. Shustek
- Improve the decsion rules for how many fake bit to add during a
  dropout. Should we make the strategy choice a variable parameter?
- Add Automatic Gain Control (AGC) on a track-by-track basis.
  That helps a lot in decoding dropouts without picking up all
  sorts of noise between data blocks that results from reducing
  the move threshold parameter that applies to all tracks.
  The AGC is computed using an exponential weighted average
  of recent peak-to-peak voltage measurements.
- Use ASTYLE's Python-like formatting to maximum algorithmic protein
  visible on a screen and minimize the clutter of syntatic sugar.

*** 27 Feb 2018, L. Shustek
- Add a parameter that does an exponential weighted average instead
  of a moving window average to track the clock rate, like we do
  for the AGC amplitude tracking.
- Changed to use Microsoft Visual Studio 2017 as the IDE. As much as
  I've liked lcc-win64 over the years, it has too many bugs and is no
  longer being supported by Jacob Navia.  That's shame; it was nice.
  The good news: code from VS in x64 "release" mode runs 3x faster!

*** 28 Feb 2018, L. Shustek
- Compensate for pulse shifting on tape by keeping track of how far
  off a pulse is from the expected time, and using some fraction of
  that (as specified by the parameter block) to adjust the window
  that determines if the next pulse is clock or data. This is a
  pretty big win for some funky blocks.

*** 4 Mar 2018, L. Shustek
- Fix AGC updating: do only at a new peak, not at each sample.
- Add AGC mode based on "minimum of the last n peaks" as an
  alternative to the "exponential average of the last n peaks".
  This helps because 2f dropouts are often preceeded by good f peaks.
- Add a filelist input to make regression testing easier.

*** 12 Mar 2018, L. Shustek
- Add NRZI decoding for 9-track 800 BPI tapes
- Add zero-band noise surpression, parametrized

*** 21 Mar 2018, L. Shustek
- Create separate parameter blocks for PE/NRZI/GCR
- Fix decoding of NRZI CRC/LRC bytes, which are often delayed
- NRZI: Create a new output file after each filemark
- add option to create SIMH .tap format output file
- implement AGC for NRZI

*** 22 Mar 2018, L. Shustek
- Fix introduced bugs: NRZI AGC, parm selection sequence
- Improve reporting of parameter set usage

*** 29 Mar 2018, L. Shustek
- MAJOR REWRITE of low-level peak detection code to switch from a hill-
  climbing algorithm to a moving-window shape detection algorithm.
  This gets us to 100% success with some test cases!
- Add NRZI pulse-shifting compensation based on expected transition
  times, similar to PE.
- Add AGC controls to the parameter block.
- Create new trace file lines for NRZI
- Remove bitrate ("br=") parameter and replace with bpi= and ips=.

*** 1 Apr 2018, L. Shustek
- Do interpolation for more accurate peak times when several
  samples are very close to the peak.
- Make the window width as a fraction of bit time be a parameter.
- Change the number of tracks from a compile-time constant to
  a run-time variable set by the "ntrks=" parameter.
- Collect and output NRZI transition timing statistics.
- Update reporting of parameter block statistics.

*** 8 Apr 2018, L. Shustek
- Add support for 7-track NRZI decoding, which has LRC but no CRC.
- Read parameter sets from an optional file at runtime.
  That allows tailoring them for particular problematical tape data.
- Add -order= option to allow arbitrary track ordering
- Add -even option for even vertical parity on 7-track BCD tapes.
- Don't decode tape labels when writing a SIMH .tap file.
- Don't display trailing blanks in standard label fields.

*** 12 Apr 2018, L. Shustek
- Rewrite the debugging trace routines to be deeply buffered, so
  we can "change history" for events that are discovered late -- for
  example, with the new moving-window algorithm, the signal peaks.
- Better display of parmsets in verbose mode.

*** 16 Apr 2018, L. Shustek
- Generalize tracing to allow multitrack voltage displays along
  with the event data.

*** 1 May 2018, L. Shustek
- Add explicit optional deskewing delays when reading raw head data:
  data for each track is independently delayed by some amount.
- Add "-deskew" option, which does a test read of the first block to
  determine the track skew values.

*** 4 May 2018, L. Shustek
- Add better trace info for NRZI decoding, since we're having problems.
- Tweak the default NRZI parameters. The deskewing is definitely a win.
- In computing skew, round up. That helps some.
- Fix bug: if skew comp took n blocks, we skipped decoding n-1 blocks.
- Add density autodetect: if BPI isn't specified, then look at the
  first few thousand transition to determine the minimum delta, and see
  if the density we derive from that is one of the standard densities.

*** 14 May 2018, L. Shustek
- Add reading of compressed .tbin binary format files, which are about
  10 times smaller and take 3-4 times less time to process!
- Add -addparity option to include 7-track parity bit with the data.
- Switch from ftell/fseek to fgetpos/fsetpos, which are more portable.
- Treat the NRZI "bits before midbit" warning as a weak error, and
  try to find a better perfect decoding that doesn't generate it.
- Move IBM standard label processing from got_datablock() into a
  a separate file, just for neatness.

*** 18 May 2018, L. Shustek
- Clean up declarations and casts to keep more stringent compilers happier.
- Fix bug that caused some NRZI blocks not to try all parmsets correctly.

*** 20 Aug 2018, L. Shustek
 - Fix bug in displaying IBM VOL1 label fields

*** 21 Aug 2018, L. Shustek
 - Fix bad test for faked bits in PE postamble that could generate
   a "bad fake data count" fatal error.
 - Change log display of parms to be exactly what could be in a .parms file.
 - Add a PE parmset with 0.1V minimum peak, for tapes with high noise floor.

*** 27 Aug 2018, L. Shustek
 - Switch back from fgetpos/fsetpos to _ftelli64/_fseeki64 in Microsoft Visual
   Visual Studio and ftello/fseeko in Linux, in yet another attempt to be
   independent of the OS. So much for the C library being portable!

*** 10 Sep 2018, L. Shustek
 - Partially implement 6250 BPI GCR decoding, which is really 9042 BPI NRZI
   with some additional redundant encoding and all sorts of bizarre stuff.
 - Refactor decoding routines into separate modules.
 - Make PKWW_RISE be a runtime parameter, and fix bug in its AGC scaling.
 - For a better longterm workflow, put all generated outflie files in the
   specified input directory; don't create a new one. Add -outfiles=xxxx
   option to change the directory<basename> for output files.
 - Fixes to va_list for better portability, thanks to Al Whaley.
 - Allow -bpi= (either 0 or value) to override what is in .tbin file header
 - Parse compile-time parmsets from strings instead of having initialized
   structures. That avoids having to keep all the structures up-to-date,
   with all parms in the right order, as parms are added.
 - For CSV files, set sample_deltat by pre-reading the first 10,000
   samples because Saleae timestamps are only given to 0.1 usec!
 - Make -l be the default, and add a -nolog option.
 - Add -textfile to create a numeric/character interpreted file, like dumptap.
 - allow the -f file list to include leading <options> for each file

 TODO:
 - figure out and implement the GCR ECC algorithms
 - make the NRZI midpoint fraction be a parameter?
 - add -skew=n,n,n  parameter to specify skew delay in clocks?
 - show summary of # of blocks that had NRZI midbit problems
   "xxx total bad blocks: xxx with parity errs, xxx malformed, xx with midbit problems"
 - add an option to base decoding on zero-crossings insteads of peaks?
*******************************************************************************/

#define VERSION "3.1"

/*
 default bit and track numbering, where 0=msb and P=parity
            on tape     in memory here    written to disk
 7-track     P012345        012345P         [P]012345
 9-track   573P21064      01234567P          01234567

 Permutation from the "on tape" sequence to the "memory here" sequence
 is done by appropriately connecting the logic analyzer probes:
    0 to 0,  1 to 1,  ...,  6 or 8 to P
 but can be overridden by using the -order command line option.
 (Note that the 7-track tapes from Al K. use -order=543210p)

 The "trkum" (or "track number") in the code corresponds to the
 "in memory here" numbering, and is the order of data in the input file.

 Note that the Qualstar 1052 9-track tape deck orders the tracks
 weirdly on the PC board as P37521064, but their labels are correct.
 */

/* code portability assumptions:
   - int is at least 32 bits
   - long long int is at least 64 bits
   - char is 8 bits
   - enumerations start at 0
   - either little-endian or big-endian is ok
*/
/*****************************************************************************
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

FILE *inf,*outf, *rlogf;
char baseinfilename[MAXPATH];  // (could have a prepended path)
char baseoutfilename[MAXPATH] = { 0 };
long long lines_in = 0, numoutbytes = 0;
int numfiles = 0, numblks = 0, numbadparityblks = 0, nummalformedblks = 0, numgoodmultipleblks = 0, numtapemarks = 0;
int numfilebytes, numfileblks;
bool logging = true, verbose = false, terse = false, quiet = false;
bool baseoutfilename_given = false;
bool filelist = false, tap_format = false;
bool tbin_file = false, txtfile = false;
bool multiple_tries = false, deskew = false, add_parity = false;
bool doing_deskew = false, doing_density_detection = false;
bool hdr1_label = false;
bool little_endian;
byte expected_parity = 1;
int input_permutation[MAXTRKS] = { -1 };
struct tbin_hdr_t tbin_hdr = { 0 };
struct tbin_dat_t tbin_dat = { 0 };

enum mode_t mode = PE;      // default
float bpi_specified = -1;   // -1 means not specified; 0 means do auto-detect
float bpi = 0;
float ips = 0;
int ntrks = 9;

enum txtfile_numtype_t txtfile_numtype = NONUM;
enum txtfile_chartype_t txtfile_chartype = NOCHAR;
int txtfile_linesize = 0;
bool txtfile_doboth;

int starting_parmset = 0;
time_t start_time;
int skip_samples = 0;
int dlog_lines = 0;

/********************************************************************
   Routines for logging and errors
*********************************************************************/
static void vlog(const char *msg, va_list args) {
   va_list args2;
   va_copy(args2, args);
   vfprintf(stdout, msg, args);  // to the console
   if (logging && rlogf)  // and maybe also the log file
      vfprintf(rlogf, msg, args2);
   va_end(args2); }

void rlog(const char* msg, ...) { // regular log
   va_list args;
   va_start(args, msg);
   vlog(msg, args);
   va_end(args); }

void debuglog(const char* msg, ...) { // debugging log
   static bool endmsg_given = false;
   va_list args;
   if (!endmsg_given) {
      if (++dlog_lines < DLOG_LINE_LIMIT) {
         va_start(args, msg);
         vlog(msg, args);
         va_end(args); }
      else {
         rlog("-----> debugging log stopped\n");
         endmsg_given = true; } } }

void vfatal(const char *msg, va_list args) {
   rlog("\n***FATAL ERROR: ");
   vlog(msg, args);
   rlog("\n");
   //rlog("I/O errno = %d\n", errno);
   exit(99); }

void fatal(const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   vfatal(msg, args);
   va_end(args); }

void assert(bool t, const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   if (!t) vfatal(msg, args);
   va_end(args); }

/********************************************************************
   Routines for processing options
*********************************************************************/
void SayUsage (void) {
   static char *usage[] = {
      "use: readtape <options> <basefilename>",
      "  the input file is <basefilename>.csv or <basefilename>.tbin",
      "  the output files will be <basefilename>.yyy.xxx",
      "    (or if -outfiles=bbb, then bbb.yyy.xxx)",
      "  the optional parameter set file is <basefilename>.parms",
      "  (or NRZI.parms, PE.parms, GCR.parms in the base or current directory)",
      "",
      "options:",
      "  -ntrks=n   set the number of tracks; default is 9",
      "  -order=    set input data order for bits 0..ntrks-2 and P, where 0=MSB",
      "             default: 01234567P for 9 trks, 012345P for 7 trks",
      "  -pe        do PE decoding",
      "  -nrzi      do NRZI decoding",
      "  -gcr       do GCR decoding",
      "  -ips=n     speed in inches/sec (default: 50, except 25 for GCR)",
      "  -bpi=n     density in bits/inch (default: autodetect)",
      "  -even      expect even parity (for 7-track NRZI BCD tapes)",
      "  -skip=n    skip the first n samples",
      "  -tap       create one SIMH .tap file from all the data",
      "  -deskew    do NRZI track deskew based on initial samples",
      "  -addparity include the parity bit in the data, if ntrks<9",
      "  -tbin      only look for a .tbin input file, not .csv",
      "  -nolog     don't create a log file",
      "  -textfile  create an interpreted .interp.txt file from the data",
      "               numeric options: -hex or -octal",
      "               character options: -ascii, -ebcdic, -bcd, or -b5500",
      "               bytes per line option: -linesize==nn",
      "  -outfiles=bbb  use bbb as the <basefilename> for output files",
      "  -m         try multiple ways to decode a block",
//    "  -l         create a log file in the output directory",
      "  -v         verbose mode (show all info)",
      "  -t         terse mode (show only bad block info)",
      "  -q         quiet mode (only say \"ok\" or \"bad\")",
      "  -f         take a file list from <basefilename>.txt",
      NULLP };
   for (int i = 0; usage[i]; ++i) fprintf(stderr, "%s\n", usage[i]); }

bool opt_key(const char* arg, const char* keyword) {
   do { // check for a keyword option and nothing after it
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   return *arg == '\0'; }

bool opt_int(const char* arg,  const char* keyword, int *pval, int min, int max) {
   do { // check for a "keyword=integer" option and nothing after it
      if (toupper(*arg++) != *keyword++)
         return false; }
   while (*keyword);
   int num, nch;
   if (sscanf(arg, "%d%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') return false;
   *pval = num;
   return true; }

bool opt_flt(const char* arg, const char* keyword, float *pval, float min, float max) {
   do { // check for a "keyword=float" option and nothing after it
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   float num;  int nch;
   if (sscanf(arg, "%f%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') return false;
   *pval = num;
   return true; }

bool opt_str(const char* arg, const char* keyword, const char** str) {
   do { // check for a "keyword=string" option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   *str = arg; // ptr to "string" part, which could be null
   return true; }

bool parse_track_order(const char*str) { // examples: P314520, 01234567P
   int bits_done = 0;
   if (strlen(str) != ntrks) return false;
   for (int i = 0; i < ntrks; ++i) {
      byte ch = str[i];
      if (toupper(ch) == 'P') ch = (byte)ntrks - 1; // we put parity last
      else {
         if (!isdigit(ch)) return false; // assumes ntrks <= 11
         if ((ch -= '0') > ntrks - 2) return false; }
      input_permutation[i] = ch;
      bits_done |= 1 << ch; }
   return bits_done + 1 == (1 << ntrks); } // must be a permutation of 0..ntrks-1

bool parse_option(char *option) { // (also called from .parm file processor)
   if (option[0] != '-') return false;
   char *arg = option + 1;
   const char *str;
   if (opt_int(arg, "NTRKS=", &ntrks, 5, 9));
   else if (opt_str(arg, "ORDER=", &str)
            && parse_track_order(str));
   else if (opt_key(arg, "NRZI")) mode = NRZI;
   else if (opt_key(arg, "PE")) mode = PE;
   else if (opt_key(arg, "GCR")) {
      mode = GCR;
      ips=25; }
   else if (opt_flt(arg, "BPI=", &bpi_specified, 100, 10000));
   else if (opt_flt(arg, "IPS=", &ips, 10, 100));
   else if (opt_int(arg, "SKIP=", &skip_samples, 0, INT_MAX)) {
      if (!quiet) rlog("The first %d samples will be skipped.\n", skip_samples); }
   else if (opt_key(arg, "TAP")) tap_format = true;
   else if (opt_key(arg, "EVEN")) expected_parity = 0;
#if DESKEW
   else if (opt_key(arg, "DESKEW")) deskew = true;
#endif
   else if (opt_key(arg, "ADDPARITY")) add_parity = true;
   else if (opt_key(arg, "TBIN")) tbin_file = true;
   else if (opt_str(arg, "OUTFILES=", &str)) {
      strncpy(baseoutfilename, str, MAXPATH);
      baseoutfilename[MAXPATH - 1] = '\0';
      baseoutfilename_given = true; }
   else if (opt_key(arg, "TEXTFILE")) txtfile = true;
   else if (opt_key(arg, "HEX")) txtfile_numtype = HEX;
   else if (opt_key(arg, "OCTAL")) txtfile_numtype = OCT;
   else if (opt_key(arg, "ASCII")) txtfile_chartype = ASC;
   else if (opt_key(arg, "EBCDIC")) txtfile_chartype = EBC;
   else if (opt_key(arg, "BCD")) txtfile_chartype = BCD;
   else if (opt_key(arg, "B5500")) txtfile_chartype = BUR;
   else if (opt_int(arg, "LINESIZE=", &txtfile_linesize, 4, MAXLINE));
   else if (opt_key(arg, "NOLOG")) logging = false;
   else if (option[2] == '\0') // single-character switches
      switch (toupper(option[1])) {
      case 'H':
      case '?': SayUsage(); exit(1);
      case 'M': multiple_tries = true;  break;
      case 'L': logging = true;  break;
      case 'T': terse = true; verbose = false;  break;
      case 'V': verbose = true; terse = quiet = false;  break;
      case 'Q': quiet = true;  terse = verbose = false;  break;
      case 'F': filelist = true;  break;
      default: goto opterror; }
   else {
opterror:  fatal("bad option: %s\n\n", option); }
   return true; }

int HandleOptions (int argc, char *argv[]) {
   /* returns the index of the first argument that is not an option;
   i.e. does not start with a dash */
   int i, firstnonoption = 0;
   //for (i = 0; i < argc; ++i) printf("arg %d: \"%s\"\n", i, argv[i]);
   for (i = 1; i < argc; i++) {
      if (!parse_option(argv[i])) { // end of switches
         firstnonoption = i;
         break; } }
   return firstnonoption; }

/********************************************************************
   utility routines
*********************************************************************/
byte parity (uint16_t val) { // compute the parity of one byte
   uint16_t p = val & 1;
   while (val >>= 1) p ^= val & 1;
   return (byte)p; }

int count_parity_errs (uint16_t *pdata, int len) { // count parity errors in a block
   int parity_errs = 0;
   for (int i=0; i<len; ++i) {
      if (parity(pdata[i]) != expected_parity) ++parity_errs; }
   return parity_errs; }

int count_faked_bits (uint16_t *pdata_faked, int len) { // count how many bits we faked for all tracks
   int faked_bits = 0;
   for (int i=0; i<len; ++i) {
      uint16_t v=pdata_faked[i];
      // This is Brian Kernighan's clever method of counting one bits in an integer
      for (; v; ++faked_bits) v &= v-1; //clr least significant bit set
   }
   return faked_bits; }

int count_faked_tracks(uint16_t *pdata_faked, int len) { // count how many tracks had faked bits
   uint16_t faked_tracks = 0;
   int c;
   for (int i=0; i<len; ++i) faked_tracks |= pdata_faked[i];
   for (c=0; faked_tracks; ++c) faked_tracks &= faked_tracks-1;
   return c; }

void show_block_errs (int len) { // report on parity errors and faked bits in all tracks
   for (int i=0; i<len; ++i) {
      byte curparity=parity(data[i]);
      if (curparity != expected_parity || data_faked[i]) { // something wrong with this data
         dlog("  %s parity at byte %4d, time %11.7lf tick %.1lf, data %02X P %d",
              curparity == expected_parity ? "good" : "bad ", i, data_time[i], TICK(data_time[i]), data[i]>>1, data[i]&1);
         if (data_faked[i]) dlog(", faked bits: %03X", data_faked[i]); //Visual Studio doesn't support %b
         dlog("\n"); } } }

void output_tap_marker(uint32_t num) {  //output a 4-byte .TAP file marker, little-endian
   for (int i = 0; i < 4; ++i) {
      byte lsb = num & 0xff;
      assert(fwrite(&lsb, 1, 1, outf) == 1, "fwrite failed in output_tap_marker");
      num >>= 8; }
   numoutbytes += 4; }

void close_file(void) {
   if (outf) {
      fclose(outf);
      if (!quiet) rlog("file closed at %.7lf with %s bytes written from %d blocks\n", timenow, intcommas(numfilebytes), numfileblks);
      outf = NULLP; } }

void create_datafile(const char *name) {
   char fullname[MAXPATH];
   if (outf) close_file();
   if (name) { // we generated a name based on tape labels
      assert(strlen(name) < MAXPATH - 5, "create_datafile name too big 1");
      strcpy(fullname, name);
      strcat(fullname, ".bin"); }
   else { // otherwise create a generic name
      assert(strlen(baseoutfilename) < MAXPATH - 5, "create_datafile name too big 1");
      if (tap_format)
         sprintf(fullname, "%s.tap", baseoutfilename);
      else sprintf(fullname, "%s.%03d.bin", baseoutfilename, numfiles+1); }
   if (!quiet) rlog("creating file \"%s\"\n", fullname);
   outf = fopen(fullname, "wb");
   assert(outf != NULLP, "file create failed for \"%s\"", fullname);
   ++numfiles;
   numfilebytes = numfileblks = 0; }

char *modename(void) {
   return mode == PE ? "PE" : mode == NRZI ? "NRZI" : mode == GCR ? "GCR" : "???"; }

/***********************************************************************************************
      tape block processing
***********************************************************************************************/

struct file_position_t {   // routines to save and restore the file position and sample time
   int64_t position;
   double time;
   int64_t time_ns; };

#if defined(_WIN32)
#define ftello _ftelli64
#define fseeko _fseeki64
#endif

void save_file_position(struct file_position_t *fp) {
   assert((fp->position=ftello(inf)) >= 0, "ftell failed");
   fp->time_ns = timenow_ns;
   fp->time = timenow; }

void restore_file_position(struct file_position_t *fp) {
   assert(fseeko(inf, fp->position, SEEK_SET) == 0, "fseek failed");
   timenow_ns = fp->time_ns;
   timenow = fp->time; }

static struct file_position_t blockstart;

void got_tapemark(void) {
   ++numtapemarks;
   save_file_position(&blockstart);
   if (!quiet) rlog("*** tapemark at file position %s and time %.7lf\n", longlongcommas(blockstart.position), timenow);
   if (txtfile) txtfile_tapemark();
   if (tap_format) {
      if (!outf) create_datafile(NULLP);
      output_tap_marker(0x00000000); }
   else if (!hdr1_label) close_file(); // not tap format: close the file if we didn't see tape labels
   hdr1_label = false; }

void got_datablock(bool malformed) { // decoded a tape block
   int length=block.results[block.parmset].minbits;
   struct results_t *result = &block.results[block.parmset];

   bool labeled = !malformed && ibm_label();
   if (length > 0 && (tap_format || !labeled)) {
      // a normal non-label data block (or in tap_format), but maybe malformed
      if (length <= 2) {
         dlog("*** ignoring runt block of %d bytes at %.7lf\n", length, timenow); }
      else if (mode != GCR || !malformed) {  // don't write malformed blocks for GCR
         //dumpdata(data, length, ntrks == 7);
         if (!outf) { // create a generic data file if we didn't see a file header label
            create_datafile(NULLP); }
         uint32_t errflag = result->errcount ? 0x80000000 : 0;  // SIMH .tap file format error flag
         if (tap_format) output_tap_marker(length | errflag); // leading record length
         for (int i = 0; i < length; ++i) { // discard the parity bit track and write all the data bits
            byte b = (byte)(data[i] >> 1);
            if (add_parity) b |= data[i] << (ntrks-1);  // optionally include parity bit as the highest bit
            assert(fwrite(&b, 1, 1, outf) == 1, "data write failed"); }
         if (tap_format) {
            byte zero = 0;  // tap format needs an even number of data bytes
            if (length & 1) assert(fwrite(&zero, 1, 1, outf) == 1, "write of odd byte failed");
            numoutbytes += 1;
            output_tap_marker(length | errflag); // trailing record length
         }
         if (txtfile) txtfile_outputrecord();
         numoutbytes += length;
         numfilebytes += length;
         ++numfileblks;
         ++numblks;
         if (mode == GCR) gcr_write_ecc_data(); // TEMP for GCR
         //if (DEBUG && (result->errcount != 0 || result->faked_bits != 0))
         //   show_block_errs(result->maxbits);
         if (DEBUG && mode == NRZI) {
            if (result->crc_bad) dlog("bad CRC: %03x\n", result->crc);
            if (result->lrc_bad) dlog("bad LRC: %03x\n", result->lrc); }
         if (result->errcount != 0) ++numbadparityblks;
         if (verbose || (terse && (result->errcount > 0 || malformed))) {
            rlog("wrote block %3d, %4d bytes, %d tries, parmset %d, max AGC %.2f, %d parity errs, ",
                 numblks, length, block.tries, block.parmset, result->alltrk_max_agc_gain, result->vparity_errs);
            if (mode == PE)
               rlog("%d faked bits on %d trks", count_faked_bits(data_faked, length), count_faked_tracks(data_faked, length));
            if (mode == NRZI) {
               if (ntrks == 9)
                  rlog("CRC %s ", result->crc_bad ? "bad," : "ok, "); // only 9-track tapes have CRC
               rlog("LRC %s ", result->lrc_bad ? "bad," : "ok, "); }
            rlog("avg speed %.2f IPS, at time %.7lf\n", 1 / (result->avg_bit_spacing * bpi), timenow); }
         if ((!quiet || terse) && malformed) {
            rlog("   malformed, with lengths %d to %d\n", result->minbits, result->maxbits);
            ++nummalformedblks; } } }
   save_file_position(&blockstart); // remember the file position for the start of the next block
   //log("got valid block %d, file pos %s at %.7lf\n", numblks, longlongcommas(blockstart.position), timenow);
};

/*****************************************************************************************
      tape data processing, in either CSV (ASCII) or TBIN (binary) format
******************************************************************************************/
// These routines are *way* faster than using sscanf!

float scanfast_float(char **p) { // *** fast scanning routines for CSV numbers
   float n=0;
   bool negative=false;
   while (**p==' ' || **p==',') ++*p; //skip leading blanks, comma
   if (**p=='-') { // optional minus sign
      ++*p;
      negative = true; }
   while (isdigit(**p)) n = n*10 + (*(*p)++ -'0'); //accumulate left of decimal point
   if (**p=='.') { // skip decimal point
      float divisor=10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ -'0')/divisor;
         divisor *= 10; } }
   return negative ? -n : n; }

double scanfast_double(char **p) {
   double n=0;
   bool negative=false;
   while (**p==' ' || **p==',') ++*p;  //skip leading blanks, comma
   if (**p=='-') { // optional minus sign
      ++*p;
      negative = true; }
   while (isdigit(**p)) n = n*10 + (*(*p)++ -'0'); //accumulate left of decimal point
   if (**p=='.') {
      double divisor=10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ -'0')/divisor;
         divisor *= 10; } }
   return negative ? -n : n; }

// While we're at it: Microsoft Visual Studio C doesn't support the wonderful POSIX %' format
// specifier for nicely displaying big numbers with commas separating thousands, millions, etc.
// So here are a couple of special-purpose routines for that.
// *** BEWARE *** THEY USE A STATIC BUFFER, SO YOU CAN ONLY DO ONE CALL PER LINE!
char *intcommas(int n) { // 32 bits
   assert(n >= 0, "bad call to intcommas: %d", n);
   static char buf[14]; //max: 2,147,483,647
   char *p = buf + 13;  int ctr = 4;
   *p-- = '\0';
   if (n == 0)  *p-- = '0';
   else while (n > 0) {
         if (--ctr == 0) {
            *p-- = ','; ctr = 3; }
         *p-- = n % 10 + '0';
         n = n / 10; }
   return p + 1; }

char *longlongcommas(long long n) { // 64 bits
   assert(n >= 0, "bad call to longlongcommas: %ld", n);
   static char buf[26]; //max: 9,223,372,036,854,775,807
   char *p = buf + 25; int ctr = 4;
   *p-- = '\0';
   if (n == 0)  *p-- = '0';
   else while (n > 0) {
         if (--ctr == 0) {
            *p-- = ',';  ctr = 3; }
         *p-- = n % 10 + '0';
         n = n / 10; }
   return p + 1; }

void reverse2(uint16_t *pnum) {
   byte x = ((byte *)pnum)[0];
   byte y = ((byte *)pnum)[1];
   ((byte *)pnum)[0] = y;
   ((byte *)pnum)[1] = x; }

void reverse4(uint32_t *pnum) {
   for (int i = 0; i < 2; ++i) {
      byte x = ((byte *)pnum)[i];
      byte y = ((byte *)pnum)[3 - i];
      ((byte *)pnum)[i] = y;
      ((byte *)pnum)[3 - i] = x; } }

void reverse8(uint64_t *pnum) {
   for (int i = 0; i < 4; ++i) {
      byte x = ((byte *)pnum)[i];
      byte y = ((byte *)pnum)[7 - i];
      ((byte *)pnum)[i] = y;
      ((byte *)pnum)[7 - i] = x; } }

void read_tbin_header(void) {  // read the .TBIN file header
   assert(fread(&tbin_hdr, sizeof(tbin_hdr), 1, inf) == 1, "can't read .tbin header");
   assert(strcmp(tbin_hdr.tag, HDR_TAG) == 0, ".tbin file missing TBINFIL tag");
   if (!little_endian)  // convert all 4-byte integers in the header to big-endian
      for (int i = 0; i < sizeof(tbin_hdr.u.s) / 4; ++i)
         reverse4(&tbin_hdr.u.a[i]);
   assert(tbin_hdr.u.s.format == TBIN_FILE_FORMAT, "bad .tbin file header version");
   assert(tbin_hdr.u.s.tbinhdrsize == sizeof(tbin_hdr),
          "bad .tbin hdr size: %d, not %d", tbin_hdr.u.s.tbinhdrsize, sizeof(tbin_hdr));
   if (!quiet) {
      rlog("reading .tbin file...\n");
      if (tbin_hdr.descr[0] != 0) rlog("   description: %s\n", tbin_hdr.descr);
      if (tbin_hdr.u.s.time_written.tm_year > 0)   rlog("   created on:   %s", asctime(&tbin_hdr.u.s.time_written));
      if (tbin_hdr.u.s.time_read.tm_year > 0)      rlog("   read on:      %s", asctime(&tbin_hdr.u.s.time_read));
      if (tbin_hdr.u.s.time_converted.tm_year > 0) rlog("   converted on: %s", asctime(&tbin_hdr.u.s.time_converted)); }
   if (tbin_hdr.u.s.ntrks != 0) {
      ntrks = tbin_hdr.u.s.ntrks;
      if (!quiet) rlog("   using .tbin ntrks = %d\n",ntrks); }
   if (tbin_hdr.u.s.mode != UNKNOWN) {
      mode = tbin_hdr.u.s.mode;
      if (!quiet) rlog("   using .tbin mode = %s\n", modename()); }
   if (bpi_specified < 0 && tbin_hdr.u.s.bpi != 0) {
      bpi = tbin_hdr.u.s.bpi;
      if (!quiet) rlog("   using .tbin bpi = %.0f\n", bpi); }
   if (ips == 0 && tbin_hdr.u.s.ips != 0) {
      ips = tbin_hdr.u.s.ips;
      if (!quiet) rlog("   using .tbin ips = %.0f\n", ips); }
   sample_deltat_ns = tbin_hdr.u.s.tdelta;
   sample_deltat = (float)sample_deltat_ns / 1e9f;
   assert(fread(&tbin_dat, sizeof(tbin_dat), 1, inf) == 1, "can't read .tbin dat");
   assert(strcmp(tbin_dat.tag, DAT_TAG) == 0, ".tbin file missing DAT tag");
   assert(tbin_dat.sample_bits == 16, "we support only 16 bits/sample, not %d", tbin_dat.sample_bits);
   if (!little_endian) reverse8(&tbin_dat.tstart); // convert to big endian if necessary
   timenow_ns = tbin_dat.tstart;
   timenow = (float)timenow_ns / 1e9; };

bool readblock(bool retry) { // read the CSV or TBIN file until we get to the end of a tape block
   // return false if we are at the endfile
   struct sample_t sample;
   bool did_processing = false;
   while (1) { // loop reading sample
      if (!retry) ++lines_in;
      if (tbin_file) { // TBIN file
         int16_t tbin_voltages[MAXTRKS];
         assert(fread(&tbin_voltages[0], 2, 1, inf) == 1, "can't read .tbin data for track 0 at time %.8lf", timenow);
         if (!little_endian) reverse2((uint16_t *)&tbin_voltages[0]);
         if (tbin_voltages[0] == -32768 /*0x8000*/) { // end of file marker
            if (did_processing)  process_sample(NULLP); // force "end of block" processing
            return false; }
         assert(fread(&tbin_voltages[1], 2, ntrks - 1, inf) == ntrks - 1, "can't read .tbin data for tracks 1.. at time %.8lf", timenow);
         if (!little_endian)
            for (int trk = 1; trk < ntrks; ++trk)
               reverse2((uint16_t *)&tbin_voltages[trk]);
         for (int trk = 0; trk < ntrks; ++trk) sample.voltage[input_permutation[trk]] = (float)tbin_voltages[trk] / 32767 * tbin_hdr.u.s.maxvolts;
         sample.time = (double)timenow_ns/1e9;
         timenow_ns += sample_deltat_ns; // (for next time)
      }
      else {  // CSV file
         char line[MAXLINE + 1];
         if (!fgets(line, MAXLINE, inf)) {
            if (did_processing)  process_sample(NULLP); // force "end of block" processing
            return false; }
         line[MAXLINE - 1] = 0;
         /* sscanf is excruciately slow and was taking 90% of the processing time!
         The special-purpose scan routines are about 25 times faster, but do
         no error checking. We replaced the following code:
         items = sscanf(line, " %lf, %f, %f, %f, %f, %f, %f, %f, %f, %f ", &sample.time,
         &sample.voltage[0], &sample.voltage[1], &sample.voltage[2],
         &sample.voltage[3], &sample.voltage[4], &sample.voltage[5],
         &sample.voltage[6], &sample.voltage[7], &sample.voltage[8]);
         assert (items == ntrks+1,"bad CSV line format"); */
         char *linep = line;
         sample.time = scanfast_double(&linep);  // get the time of this sample
         for (int i = 0; i < ntrks; ++i) // read voltages for all tracks, and permute as necessary
            sample.voltage[input_permutation[i]] = scanfast_float(&linep); }
      timenow = sample.time;
      if (torigin == 0) torigin = timenow; // for debugging output

      if (!block.window_set) { //
         // we know the sample delta time, so set the width of the peak-detect moving window
         if (bpi)
            pkww_width = min(PKWW_MAX_WIDTH, (int)(PARM.pkww_bitfrac / (bpi*ips*sample_deltat)));
         else pkww_width = 8; // a random reasonable choice if we don't have BPI specified
         static bool said_rates = false;
         if (!quiet && !said_rates) {
            rlog("%s, %d BPI, %d IPS, sampling rate is %s Hz (%.2f usec), peak window width is %d samples (%.2f usec)\n",
                 modename(), (int)bpi, (int)ips, intcommas((int)(1.0 / sample_deltat)), sample_deltat*1e6, pkww_width, pkww_width * sample_deltat*1e6);
            said_rates = true; }
         block.window_set = true; }

      if (process_sample(&sample) != BS_NONE)  // process one voltage sample point for all tracks
         break;  // until we recognize an end of block
      did_processing = true; }
   return true; }

/***********************************************************************************************
   file processing
***********************************************************************************************/

// process a complete input file whose path and base file name are in baseinfilename[]
// return TRUE if all blocks were well-formed and error-free

bool process_file(int argc, char *argv[]) {
   char filename[MAXPATH], logfilename[MAXPATH];
   char line[MAXLINE + 1];
   bool ok = true;

#if 0 // we no longer create a directory
#if defined(_WIN32)
   if (_mkdir(baseoutfilename) != 0) // create the working directory for output files
#else
   if (mkdir(baseoutfilename, 0777) != 0) // create the working directory for output files
#endif
      assert(errno == EEXIST || errno == 0, "can't create directory \"%s\", baseoutfilename");
#endif

   if (logging) { // Open the log file
      sprintf(logfilename, "%s.log", baseoutfilename);
      assert((rlogf = fopen(logfilename, "w")) != NULLP, "Unable to open log file \"%s\"", logfilename); }

   filename[MAXPATH - 5] = '\0';
   if (!tbin_file) {
      strncpy(filename, baseinfilename, MAXPATH - 5);  // try to open <baseinfilename>.csv
      strcat(filename, ".csv");
      inf = fopen(filename, "r"); }
   if (!inf) {
      strncpy(filename, baseinfilename, MAXPATH - 6);  // try to open <baseinfilename>.tbin
      strcat(filename, ".tbin");
      inf = fopen(filename, "rb");
      assert(inf != NULLP, "Unable to open input file \"%s\" .csv or .tbin", baseinfilename);
      tbin_file = true; }
   assert(!add_parity || ntrks < 9, "-parity not allowed with ntrks=%d", ntrks);
   if (!quiet) {
      rlog("this is readtape version \"%s\", compiled on %s %s, running on %s",
           VERSION, __DATE__, __TIME__, ctime(&start_time)); // ctime ends with newline!
      rlog("command line: ");
      for (int i = 0; i < argc; ++i)  // for documentation, show invocation options
         rlog("%s ", argv[i]);
      rlog("\n");
      rlog("this is a %s-endian computer\n", little_endian ? "little" : "big");
      rlog("the current directory is %s\n", _getcwd(line, MAXLINE));
      rlog("the output files will be %s.xxx\n", baseoutfilename);
      rlog("reading file %s\n", filename); }
   if (tbin_file) read_tbin_header();  // sets NRZI, etc., so do before read_parms()

   read_parms(); // read the .parm file, if any

   if (!tbin_file) {
      // first two (why?) lines in the input file are headers from Saleae
      assert(fgets(line, MAXLINE, inf) != NULLP, "Can't read first CSV title line");
      assert(fgets(line, MAXLINE, inf) != NULLP, "Can't read second CSV title line");
      // For CSV files, set sample_deltat by reading the first 10,000 samples, because Saleae timestamps
      // are only given to 0.1 usec. If the sample rate is, say, 3.125 Mhz, or 0.32 usec between samples,
      // then all the timestamps are off by either +0.08 usec or -0.02 usec!
      struct file_position_t filestart;
      save_file_position(&filestart); // remember where we are starting
      int linecounter = 0;
      double first_timestamp = -1;
      while (fgets(line, MAXLINE, inf) && ++linecounter < 10000) {
         char *linep = line;
         double timestamp = scanfast_double(&linep);
         if (first_timestamp < 0) first_timestamp = timestamp;
         else sample_deltat = (float)((timestamp - first_timestamp)/(linecounter-1)); }
      restore_file_position(&filestart); // go back to reading the first sample
      //rlog("sample_delta set to %.2f usec after %s samples\n", sample_deltat*1e6, intcommas(linecounter));
   }

   if (skip_samples > 0) {
      if (!quiet) rlog("skipping %d samples\n", skip_samples);
      while (skip_samples--) {
         bool endfile;
         struct sample_t sample;
         if (tbin_file) endfile = fread(sample.voltage, 2, ntrks, inf) != ntrks;
         else endfile = !fgets(line, MAXLINE, inf);
         assert(!endfile, "endfile with %d lines left to skip\n", skip_samples); } }
   interblock_expiration = 0;
   starting_parmset = 0;

   if (ips == 0) ips = 50;  // default
   if (bpi_specified >= 0) bpi = bpi_specified;  // command line overrides tbin header
   if (mode == GCR) {
      if (bpi != 9042) rlog ("BPI was reset to 9042 for GCR 6250\n");
      bpi = 9042; } // the real BPI isn't 6250!

   if (bpi == 0) {  // **** auto-detect the density by looking at how close transitions are at the start of the tape
      doing_density_detection = true;
      estden_init();
      int nblks = 0;
      struct file_position_t filestart;
      save_file_position(&filestart); // remember the file position for the start of the file
      do {
         init_blockstate(); // do one block
         block.parmset = starting_parmset;
         init_trackstate();
         if (!readblock(true)) break; // stop if endfile
         ++nblks; }
      while (!estden_done()); // keep going until we have enough transitions
      //estden_show();
      estden_setdensity(nblks);
      restore_file_position(&filestart);
      interblock_expiration = 0;
      doing_density_detection = false; }

#if DESKEW     // ***** automatic deskew determination based on the first few blocks
   if (mode != PE && deskew) { // currently only for NRZI or GCR
      doing_deskew = true;
      if (verbose) rlog("preprocessing to determine head skew\n");
      int nblks = 0;
      struct file_position_t filestart;
      save_file_position(&filestart); // remember the file position for the start of the file
      do { // do one block at a time
         init_blockstate();
         block.parmset = starting_parmset;
         init_trackstate();
         if (!readblock(true)) break; // stop if endfile
         ++nblks; } // keep going until we have enough transitions or have processed too many blocks
      while (nblks < MAXSKEWBLKS && skew_min_transitions() < MINSKEWTRANS);
      if (!quiet) rlog("skew compensation after reading %d blocks:\n", nblks);
      skew_set_deskew();
      restore_file_position(&filestart);
      interblock_expiration = 0;
      doing_deskew = false; }
#endif

   while (1) { // keep processing lines of the file
      init_blockstate();  // initialize for a new block
      block.parmset = starting_parmset;
      save_file_position(&blockstart); // remember the file position for the start of a block
      dlog("\n*** block start file pos %s at %.7lf\n", longlongcommas(blockstart.position), timenow);

      bool keep_trying;
      int last_parmset;
      block.tries = 0;

#if 0 // TEMP Scan for optimal sets of GCR parms for the first block
      if (numblks == 0) {
         int clk_window = 0; //for (int clk_window = 10; clk_window <= 30; clk_window += 5)
         for (float clk_alpha = 0.010f; clk_alpha <= 0.030f; clk_alpha += 0.002f)
            for (float pulse_adj = 0.2f; pulse_adj <= 0.401f; pulse_adj += 0.1f)
               for (float z1pt = 1.4f; z1pt <= 1.501f; z1pt += .01f)
                  for (float z2pt = 2.20f; z2pt <= 2.401f; z2pt += .02f) {
                     PARM.clk_window = clk_window;
                     PARM.clk_alpha = clk_alpha;
                     PARM.pulse_adj = pulse_adj;
                     PARM.z1pt = z1pt;
                     PARM.z2pt = z2pt;
                     init_trackstate();
                     extern int first_parity_err;
                     first_parity_err = -1;
                     readblock(true);
                     //rlog("with clk_window %d pulseadj %.3f z1pt %.3f z2pt %.3f firsterr %d\n",
                     //   clk_window, pulse_adj, z1pt, z2pt, first_parity_err);
                     rlog("clk_alpha %.3f pulseadj %.3f z1pt %.3f z2pt %.3f firsterr %d ",
                          clk_alpha, pulse_adj, z1pt, z2pt, first_parity_err);
                     int countgood = 0;
                     for (int trk = 0; trk < 9; ++trk)
                        if (trkstate[trk].datacount == 5743 || trkstate[trk].datacount == 5745) ++countgood;
                     rlog("goodtrks %d parityerrs %d\n", countgood, block.results[block.parmset].errcount);
                     restore_file_position(&blockstart); // go back to try again
                     interblock_expiration = 0; } }
      // copy and paste the log lines into Excel using the Text Import Wizard, then sort as desired
#endif
      do { // keep reinterpreting a block with different parameters until we get a perfect block or we run out of parameter choices
         keep_trying = false;
         ++PARM.tried;  // note that we used this parameter set in another attempt
         last_parmset = block.parmset;
         //window_set = false;
         init_trackstate();
         dlog("\n     trying block %d with parmset %d at byte %s at time %.7lf\n", numblks + 1, block.parmset, longlongcommas(blockstart.position), timenow);
         if (!readblock(block.tries>0)) goto endfile; // ***** read a block ******
         struct results_t *result = &block.results[block.parmset];
         result->warncount = result->missed_midbits;
         if (result->blktype == BS_NONE) goto endfile; // stuff at the end wasn't a real block
         ++block.tries;
         dlog("     block %d is type %d parmset %d, minlength %d, maxlength %d, %d errors, %d faked bits at %.7lf\n", //
              numblks + 1, result->blktype, block.parmset, result->minbits, result->maxbits, result->errcount, result->faked_bits, timenow);
         if (result->blktype == BS_TAPEMARK) goto done;  // if we got a tapemake, we're done
         if (result->blktype == BS_BLOCK && result->errcount == 0 && result->warncount == 0 && result->faked_bits == 0) { // if we got a perfect block, we're done
            if (block.tries>1) ++numgoodmultipleblks;  // bragging rights; perfect blocks due to multiple parameter sets
            goto done; }
         if (multiple_tries &&  // if we're supposed to try multiple times
               (mode != PE || result->minbits != 0)) { // and there are no dead PE tracks (which probably means we saw noise)
            int next_parmset = block.parmset; // then find another parameter set we haven't used yet
            do {
               if (++next_parmset >= MAXPARMSETS) next_parmset = 0; }
            while (next_parmset != block.parmset &&
                   (parmsetsptr[next_parmset].active == 0 || block.results[next_parmset].blktype != BS_NONE));
            //log("found next parmset %d, block_parmset %d, keep_trying %d\n", next_parmset, block.parmset, keep_trying);
            if (next_parmset != block.parmset) { // we have a parmset, so can try again
               keep_trying = true;
               block.parmset = next_parmset;
               dlog("   retrying block %d with parmset %d at byte %s at time %.7lf\n", numblks + 1, block.parmset, longlongcommas(blockstart.position), timenow);
               restore_file_position(&blockstart);
               interblock_expiration = 0; } } }
      while (keep_trying);

      // We didn't succeed in getting a perfect decoding of the block, so pick the best of multiple bad decodings.

      if (block.tries == 1) { // unless we don't have multiple decoding tries
         if (block.results[block.parmset].errcount > 0) ok = false; }
      else {
         dlog("looking for good parity blocks\n");
         int min_bad_bits = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 1: find a decoding with no errors and the minimum number of faked bits or warnings
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->errcount == 0 && (result->faked_bits + result->warncount) < min_bad_bits) {
               min_bad_bits = result->faked_bits + result->warncount;
               block.parmset = i;
               dlog("  best good parity choice is parmset %d\n", block.parmset); } }
         if (min_bad_bits < INT_MAX) goto done;

         ok = false; // we had at least one bad block
         dlog("looking for minimum bad parity blocks\n");
         min_bad_bits = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 2: Find the decoding with the mininum number of errors
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->errcount < min_bad_bits) {
               min_bad_bits = result->errcount;
               block.parmset = i;
               dlog("  best bad parity choice is parmset %d\n", block.parmset); } }
         if (min_bad_bits < INT_MAX) goto done;

         dlog("looking for least malformed blocks\n");
         int min_track_diff = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 3: The block is malformed; find the decoding with the minimum difference in track lengths
            struct results_t *result = &block.results[i];
            int track_diff = result->maxbits - result->minbits;
            if (result->blktype == BS_MALFORMED && track_diff < min_track_diff) {
               min_track_diff = track_diff;
               block.parmset = i;
               dlog("  best malformed block choice is parmset %d\n", block.parmset); } }
         assert(min_track_diff < INT_MAX, "bad malformed block status"); }
done:
      dlog("  chose parmset %d as best after %d tries\n", block.parmset, block.tries);
      ++PARM.chosen;  // count times that this parmset was chosen to be used
      if (block.tries>1 // if we processed the block multiple times
            && last_parmset != block.parmset) { // and the decoding we chose isn't the last one we did
         restore_file_position(&blockstart);    // then reprocess the chosen one to recompute that best data
         interblock_expiration = 0;
         dlog("     rereading block %d with parmset %d at byte %s at time %.7lf\n", numblks + 1, block.parmset, longlongcommas(blockstart.position), timenow);
         init_trackstate();
         assert(readblock(true), "got endfile rereading a block");
         struct results_t *result = &block.results[block.parmset];
         dlog("     reread of block %d with parmset %d is type %d, minlength %d, maxlength %d, %d errors, %d faked bits at %.7lf\n", //
              numblks + 1, block.parmset, result->blktype, result->minbits, result->maxbits, result->errcount, result->faked_bits, timenow); }
      if (mode ==NRZI && !doing_deskew && block.results[block.parmset].missed_midbits > 0)
         rlog("*** WARNING: %d bits were before the midbit using parmset %d for block %d at %.7lf\n",
              block.results[block.parmset].missed_midbits, block.parmset, numblks + 1, timenow);

      switch (block.results[block.parmset].blktype) {  // process the block according to our best decoding
      case BS_TAPEMARK:
         got_tapemark();
         break;
      case BS_BLOCK:
         got_datablock(false);
         break;
      case BS_MALFORMED:
         got_datablock(true);
         break;
      default:
         fatal("bad block state after decoding", ""); }

#if USE_ALL_PARMSETS
      do { // If we start with a new parmset each time, we'll use them all relatively equally and can see which ones are best
         if (++starting_parmset >= MAXPARMSETS) starting_parmset = 0; }
      while (parmsetsptr[starting_parmset].clk_factor == 0);
#else
// otherwise we always start with parmset 0, which is configured to be the best for most tapes
#endif

   }  // next line of the file
endfile:
   rlog("endfile at time %.7lf\n", timenow);
   if (tap_format && outf) output_tap_marker(0xffffffffl);
   if (txtfile) txtfile_close();
   close_file();
   return ok; }

void breakpoint(void) { // for the debugger
   static int counter;
   ++counter; }

/*-----------------------------------------------------------------
  The basic structure for processing the data is:

 main:
 process_file()
   until EOF do
       init_block()
       for all parameter sets do
          init_tracks()
          readblock: do process_sample() until end_of_block()
      pick the best decoding
      if necessary to regenerate a previous better decoding,
         init_tracks()
         readbock: do process_sample() until end_of_block()
      call got_datablock() or got_tapemark()
         decode standard labels, if any
         write output file data block
---------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
   int argno;
   char *cmdfilename;

#if 0 // compiler checks
#define showsize(x) printf(#x "=%d bytes\n", (int)sizeof(x));
   showsize(byte);
   showsize(bool);
   showsize(int);
   showsize(long);
   showsize(long long);
   showsize(struct parms_t);
#endif
#if 0 // show arguments
   for (int i = 0; i < argc; ++i) printf("arg %d:%s\n", i, argv[i]);
#endif
   uint32_t testendian = 1;
   little_endian = *(byte *)&testendian == 1;

   // process command-line options

   if (argc == 1) {
      SayUsage();
      exit(4); }
   argno = HandleOptions(argc, argv);
   if (argno == 0) {
      fprintf(stderr, "\n*** No <basefilename> given\n\n");
      SayUsage();
      exit(4); }
   if(argc > argno+1) {
      fprintf(stderr, "\n*** unknown parameter: %s\n\n", argv[argno]);
      SayUsage();
      exit(4); }

   // set option defaults if not specified

   if (input_permutation[0] == -1) // no input value permutation was given
      for (int i = 0; i < ntrks; ++i) input_permutation[i] = i; // create default
   cmdfilename = argv[argno];

   //TODO: Move what follows into process_file so we do it for each file of a list?
   // Nah. A more elegant solution would be to gather all the options into a
   // structure, then save/restore it around the processing for each file, so it
   // inherits what was given on the command line but can add/overrride.
   if (!baseoutfilename_given) strcpy(baseoutfilename, cmdfilename);
   if (txtfile) {
      if (txtfile_chartype == NOCHAR && txtfile_numtype == NONUM) {
         txtfile_chartype = ASC; txtfile_numtype = HEX; }
      txtfile_doboth = txtfile_chartype != NOCHAR && txtfile_numtype != NONUM;
      if (txtfile_linesize == 0) txtfile_linesize = txtfile_doboth ? 40 : 80; }

   start_time = time(NULL);
   if (filelist) {  // process a list of files
      char filename[MAXPATH];
      strncpy(filename, cmdfilename, MAXPATH - 5);
      filename[MAXPATH - 5] = '\0';
      strcat(filename, ".txt");
      FILE *listf = fopen(filename, "r");
      assert(listf != NULLP, "Unable to open file list file \"%s\"", filename);
      char line[MAXLINE + 1];
      while (fgets(line, MAXLINE, listf)) {
         line[strcspn(line, "\n")] = 0;
         char *ptr = line;
         if (*ptr != 0) {
            skip_blanks(&ptr);
            while (*ptr == '-') { // parse leading options for this file
               char option[MAXLINE];
               assert(getchars_to_blank(&ptr, option), "bad option string in file list: %s", ptr);
               assert(parse_option(option), "bad option in file list: %s", option);
               skip_blanks(&ptr); }
            strncpy(baseinfilename, ptr, MAXPATH - 5); // copy the filename, which could include a path
            baseinfilename[MAXPATH - 5] = '\0';
            bool result = process_file(argc, argv);
            printf("%s: %s\n", baseinfilename, result ? "ok" : "bad"); } } }

   else {  // process one file
      strncpy(baseinfilename, cmdfilename, MAXPATH - 5);
      baseinfilename[MAXPATH - 5] = '\0';
      bool result = process_file(argc, argv);
      if (quiet) {
         printf("%s: %s\n", baseinfilename, result ? "ok" : "bad"); }
      else {
         double elapsed_time = difftime(time(NULL), start_time); // integer seconds!?!
         rlog("\n%s samples processed in %.0lf seconds, or %.2lf seconds/block\n",
              longlongcommas(lines_in), elapsed_time, numblks == 0 ? 0 : elapsed_time / numblks);
         rlog("detected %d tape marks\n", numtapemarks);
         rlog("created %d files with %s bytes and %d data blocks of which %d had errors and %d were malformed.\n",
              numfiles, longlongcommas(numoutbytes), numblks, numbadparityblks, nummalformedblks); } }

   if (!quiet) { // show result statistics
      rlog("%d perfect blocks needed to try more than one parmset\n", numgoodmultipleblks);
      for (int i = 0; i < MAXPARMSETS; ++i) //  // show stats on all the parameter sets we tried
         if (parmsetsptr[i].tried > 0) {
            rlog("parmset %d was tried %4d times and used %4d times, or %5.1f%%\n",
                 i, parmsetsptr[i].tried, parmsetsptr[i].chosen, 100.*parmsetsptr[i].chosen / parmsetsptr[i].tried);
#if 0 // forget trying to summarize the parms here; it's too hard to keep up with the changes!
            if (parmsetsptr[i].clk_window) rlog("clk wind %d bits, ", parmsetsptr[i].clk_window);
            else if (parmsetsptr[i].clk_alpha) rlog("clk alpha %.2f, ", parmsetsptr[i].clk_alpha);
            else rlog("clk spacing %.2f usec, ", (mode == PE ? 1/(ips*bpi) : nrzi.clkavg.t_bitspaceavg)*1e6);
            if (parmsetsptr[i].agc_window) rlog("AGC wind %d bits, ", parmsetsptr[i].agc_window);
            else if (parmsetsptr[i].agc_alpha) rlog("AGC alpha %.2f, ", parmsetsptr[i].agc_alpha);
            else rlog("AGC off");
            if (mode == PE)
               rlog("clk factor %.2f, ", parmsetsptr[i].clk_factor);
            rlog("min peak %.2fV, pulse adj %.2f, ww frac %.2f, ww rise %.2fV",
                 parmsetsptr[i].min_peak, parmsetsptr[i].pulse_adj, parmsetsptr[i].pkww_bitfrac, parmsetsptr[i].pkww_rise);
#endif
         } }
#if PEAK_STATS
   if (!quiet) output_peakstats();
#endif
   return 0; }

//*
