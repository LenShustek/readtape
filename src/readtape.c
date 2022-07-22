//file: readtape.c
/*******************************************************************************

Read the analog signals recorded from the heads of a magnetic tape drive,
decode the data in any of several formats and speeds,
and create one or more files with the original data written to the tape.

readtape <options> <baseinfilename>

The input is either:
  - a CSV file with multiple columns: a timestamp in seconds,
    and then the read head voltages at that time for each of the tracks.
  - a TBIN binary compressed data file; see csvtbin.h

This is open-source code at https://github.com/LenShustek/readtape.
- See the "A_documentation.txt" file for a narrative about usage,
  internal operation, and the algorithms we use.
- See VCF_Aug2020_01.pdf for a slide show about the system.
- See https://www.youtube.com/watch?v=7YoolSAHR5w&t=4200s for a bad-quality
  video of me giving a talk using that slideshow.

********************************************************************************
Copyright (C) 2018,2019,2022 Len Shustek
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
********************************************************************************

---CHANGE LOG ---

*** 20 Jan 2018, L. Shustek, Started first version

*** 05 Feb 2018, L. Shustek, First github posting, since it kinda works.

*** 09 Feb 2018, L. Shustek
- Replace sscanf for a 20x speedup!
- Made major changes to the decoding algorithm:
 - In the voltage domain, do idle detection based on peak-to-peak
   voltage, not proximity to a baseline resting level.
 - In the time domain, do clock simulation when the signal drops out.
   In other words, we fake bits when we see no flux transitions.

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
  longer being supported by Jacob Navia. That's shame; it was nice.
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

 *** 10 Oct 2018, L. Shustek
- Make PKWW_RISE be a runtime parameter, and fix bug in its AGC scaling.
- For a better longterm workflow, put all generated output files in the
  specified input directory; don't create a new one. Add -outfiles=xxxx
  option to change the directory<basename> for output files.
- Fixes to va_list for better portability, thanks to Al Whaley.
- Parse compile-time parmsets from strings instead of having initialized
  structures. That avoids having to keep all the structures up-to-date,
  with all parms in the right order, as parms are added.
- Allow -bpi and -ips (either 0 or value) to override what is in .tbin file header
- For CSV files, set sample_deltat by pre-reading the first 10,000
  samples because Saleae timestamps are only given to 0.1 usec!
- Make -l be the default, add a -nolog option, remove -t (terse).
- Add -textfile to create a numeric/character interpreted file, like dumptap.
- allow the -f file list to include leading <options> for each file, except
  that not all options will work when given there

 *** 17 Oct 2018, L. Shustek
- Change GCR decoding from NRZI-style midbit estimation to inferring the
  zeroes based on peak delta times, since there are at most 2 zeros.
- Make the NRZI midpoint fraction be a parameter
- Rationalize block error and warning counts, including mismatched tracks
- Add ECC checking; many thanks to Tom Howell for analyzing the algorithm
  and providing the snippet of code we needed!
- Make NRZI decoding be more robust to the late detection of shallow peaks
  by moving the midbit assessment to the next block boundary

*** 10 Nov 2018, L. Shustek
- Implement -correct: when there's an NRZI or GCR parity error, check if one
  track is much weaker (higher AGC) than all the others at that time, and if so,
  correct the parity by changing that track's bit.
- Set/check ntrks from .tbin header or .CSV title line
- If reading from .tbin and !TBIN_NO_ORDER, ignore -order parm with warning,
  so that the same batch file can be used for either kind of file

*** 12 Dec 2018, L. Shustek
- Add "skew=n,n,n" parameter to manually specify head skew
- Add -SDS text interpretation, and make the filename indicate decoding options

*** 21 Mar 2019, L. Shustek, V3.6
- Add -zeros option to base decoding on zero crossings instead of peaks
  (That makes some variable names and comments that use "peak" misleading.)
- Assume -txtfile if any of its suboptions were given
- Add -linefeed suboption to -txtfile
- Incorporate the GCR ECC correction code from Tom Howell. Thanks, Tom!

*** 1 Aug 2019, L. Shustek, V3.7
- separate min_peak parameter from ZEROCROSS_PEAK, which can't be zero
- fix broken generation of filenames from IBM standard labels
- fix NRZI clock realignment when seeing 1-bits in the CRC/LRC area
- allow PE bits to be reversed polarity, based on the polarity of the
  first bit that starts the preamble (Depends on logic analyzer setup.)
- add -SDSM character interpretation, although we don't have examples
- elaborate debug and verbose booleans into various numeric levels
  (partially implemented)
- add -outp to specify output directory pathname

*** 5 Dec 2019, L. Shustek, V3.8
- major additions to support Whirlwind 6-track quasi-NRZI format,
  which also causes us to distinguish heads from tracks.
- add subsample=n to speed up processing of oversampled data
- add -FLEXO Flexowriter and -octal2 2-byte 16-bit octal decoding
- add -invert, -fluxdir={POS|NEG|AUTO}
- change verbose_level and debug_level into flag bits
- make opt_int accept 0x.., 0b.., and 0.. constants for flags

*** 23 Dec 2019, L. Shustek, V3.9
- Whirlwind: do auto-polarity detection, and remap top/bottom to start/end
- Whirlwind: base decoding on clock end pulses, not clock start pulses
- Make PARM.min_peak be relative to avg_height and agc_gain. Why did we not, before?
  (Does that break other modes?)
- Add -sumt and -sumc to accumulate statistics for multiple files
- Add -showibg=nnn to show large interblock gaps
- Allow command line to override .tbin -order string

*** 27 Feb 2022, L. Shustek, V3.10
- Add the "differentiate" option and a new zero-crossing algorithm for it.
  (Thanks to Chuck Sobey for providing the GCR data that inspired it.)
- Change max IPS from 100 to 200.
- Change timestamp display from 7 to 8 digits, for sampling rates above 10 Mhz.
- Fix bug introduced in 3.8: textfile shows an extra 3 characters at block end.
- Minor log changes: always log first/last blocks; show blocks so far at tape mark.

*** 18 April 2022, L. Shustek, V3.11
- Add the -ADAGE and -ADAGETAPE character sets for the Adage Graphics Terminal
- Allow the LRC for 7-track tapes to be 3-5 characters after the block, not just 4,
  because that's what Adage did, contrary to the spec.
- Have octal display in the textfile show only 2 octal digits when there are 7 tracks or fewer

*** 12 June 2022, L. Shustek, V3.12
- Add the -revparity=n option to reverse the expected parity for label records.
- Experiment with aborting NRZI end-block processing when spurious 1-bits are found,
  to avoid creating bogus records. It's only occasionally successful, though.
- Experiment with incrementally adjusting the head skew at the end of each block
  if -adjskew is specified. It needs work, though, and isn't currently useful.
- Add -cdcdisplay and -cdcfield as character decodings
- Fix tapemark decoding, which we broke maybe in V3.11

*** 20 June 2022, L. Shustek, V3.13
- Change -cdcdisplay to -cdc, and -cdcfield to -univac.
- Add -tapread option to only read .tap files and do text/numeric decoding.
- Relax timing for NRZI tapemark detection.

*** 24 June 2022, L. Shustek, V3.14
- Make -ASCII ignore the top (0x80) bit
- Changes to -tapread:
    - show just record summaries if neither text nor numeric mode is specified.
      (also for when it isn't -tapread)
    - treat the end of file as an implicit EOM marker, with a warning
    - allow variable data record padding; look for the trailing length to indicate the end

*** 18 July 2022, L. Shustek, V3.15
- Fix NRZI bug: a second bogus peak in a single bit window caused the extra bit to be propagated on
  that track for the rest of the block. Now in zerocheck() we delete a second peak in one window.
- In the textfile, expand the info about the block into a line before the data like this:
      block nnnn: xxxx bytes at time yyyy, <error information>
- In the textfile, have "tape mark" show the time.
- By default comment on gaps greater than 5 seconds, ie -showibg=5000.
- Allow extensions .csv, .tbin, .tap (or others with -tapread) to be given on the command line.

*** 11 July 2022, L. Shustek, V3.16
 - Fix -tapread: filename parsing; trying to show info not in the .tap file.

 TODO:
- support reading Saleae binary export files;
  see https://support.saleae.com/faq/technical-faq/data-export-format-analog-binary
  (But: .tbin is still smaller and faster, so have csvtbin do that conversion too?)
  (But: Saleae has changed their export format, so which to do?)
- make multiple decodes work for WW. Pb is that the peak and skew state
  needs to be saved and restored because the blocks can be so close.
  Flash: we now have similar code at the end of deskewing, so check that out.
- make zerocrossing work for PE and NRZI (but: need examples to test with)
- continue implementing the new debug_level and verbose_level controls
- write some kind of block for undecodeable blocks? (Naaah...)
- check if generating faked bits still works right (mostly for PE);
  we're likely to have broken it with all other changes
- figure out and implement the GCR CRC algorithms (or convince Tom Howell to!)
- check that providing a list of multiple input files still works ok
- to correctly process multiple files of different kinds: collect file-
  specific globals into a structure that is easily reinitialized, and inherits
  global command-line options that can be overridden from the .parm file.
***********************************************************************************/

#define VERSION "3.16"

/*  the default bit and track numbering, where 0=msb and P=parity
             on tape     our tracks   in memory here    exported data
            physically   ntrk-1..0    uint16_t data[]
 6-track WW   CMLcml      mlcMLC       MLP (best)     MLMLMLML (from 4 consecutive tape characters)
 7-track     P012345      012345P       012345P        [P]012345
 9-track   573P21064     01234567P     01234567P      [P]01234567

 (For Whirlwind, which redundantly encodes 2 bits of data in each tape character,
  M=MSB, L=LSB, and the parity bit in memory is bogus. Also note that the order
  on the tape can be different, depending on which drive wrote the tape!)

 Permutation from the "on tape" sequence to the "memory here" sequence
 is done by appropriately connecting the logic analyzer probes as
   0 to trk 0, 1 to trk 1, ...,  6 or 8 to the parity track P
 But the order can be overridden by using the -order command line option.
 (Note that many 7-track tapes from Al K use -order=543210p)

 The "trkum" (or "track number") in this code corresponds to the
 "in memory here" numbering, and is the order of data in the input file.

 Note that the Qualstar 1052 9-track tape deck orders the tracks
 weirdly on the PC board as P37521064, which is not the physical track
 order, but their labels on the board are correct.

 TIP: If you have the CSV file but not the Salae data file, use BeSpice Wave to view it.
 AnalogFlavor, http://www.analogflavor.com/en/bespice/bespice-wave/
 (download the free trial from the MS app store, https://www.microsoft.com/en-us/p/bespicewave/9pg67lv956hh
 because the msi-installed one from AnalogFlavor's website fails loading big CSV files)

 code portability assumptions:
   - int is at least 32 bits
   - long long int is at least 64 bits
   - char is 8 bits
   - enumerations start at 0
   - either little-endian or big-endian is ok
*/

/*---------- OUTLINE OF THE CODE STRUCTURE  -----------------
main
   process options
   if filelist
      loop // all files
         read options, filename
         process_file()
   else
      process_file()
      show file stats
   show summary stats

process_file, return bool "all blocks good"
   open log file, input file
   read_parms()
   if CSV, preread 10,000 samples to get delta
   if skip=, skip samples
   if bpi=0, save_file_position()
      do until enough transitions or endfile
         init_blockstate(), init_trackstate(), readblock()
      estimate density, restore_file_position()
   if deskew, save_file_position()
      do until enough transitions or endfile
         init_blockstate(), init_trackstate(), readblock()
      compute skew, restore_file_position()
   loop // all blocks
      init_blockstate(), save_file_position()
      loop // all tries
         init_trackstate()
         readblock(); if endfile or BS_NONE, exit; if BS_NOISE, ignore
         if BS_TAPEMARK or BS_BLOCK perfect, goto done
         restore_file_position()
      pick best decoding
      done: if chosen decoding isn't the last we did
         restore_file_position()
         init_trackstate()
         readblock(), endfile is error(?)
      BS_TAPEMARK: got_tapemark()
      BS_BLOCK: got_datablock(good)
                   write output data to file
                   show info and warnings about the block
      BS_BADBLOCK: got_datablock(bad)
                     show warning about bad block

readblock, return bool "not endfile"
   loop // lots of samples
      read sample from the file
      if endfile
         if did_processing, force_end_of_block()
         endfile=true
         goto done
      did_processing = true;
      while process_sample(sample) == BS_NONE
   done: sum errors
   return not endfile

process_sample, return block status
   deskew tracks
   if not interblock
      if time, nrzi_zerocheck()
         if post_counter > 8, nrzi_end_of_block
             nrzi_postprocess()
               interblock = xxx
            blktype = yyy
      detect peaks
         call {pe,nrzi,gcr}_{top,bot}()
      if GCR idle, gcr_end_of_block()
            gcr_postprocess()
            interblock = xxx
            blktype = yyy
      if PE idle, pe_end_of_block()
            blktype = yyy
   if still interblock, return BS_NONE
   else return blktype
-----------------------------------------------------*/

#include "decoder.h"

// file names and handles
FILE *inf, *outf, *rlogf = NULL, *summf;
char baseinfilename[MAXPATH];  // (could have a prepended path)
char baseoutfilename[MAXPATH] = { 0 };
char outpathname[MAXPATH] = { 0 };
char summtxtfilename[MAXPATH] = { 0 };
char summcsvfilename[MAXPATH] = { 0 };
char outdatafilename[MAXPATH], indatafilename[MAXPATH];

// statistics for the whole tape
int numblks = 0, numblks_err = 0, numblks_warn = 0, numblks_trksmismatched = 0, numblks_midbiterrs = 0;
int numblks_goodmultiple = 0, numblks_unusable = 0, numblks_corrected = 0;
int numblks_limit = INT_MAX;
int numfiles = 0, numtapemarks = 0, num_flux_polarity_changes = 0;
long long lines_in = 0, numdatabytes = 0, numoutbytes = 0;
long long numsamples = 0;

// statistics for a particular output file
int numfileblks;
long long numfilebytes;

// Other globals. Some could move to a file-specific structure so when a file list
// is provided we can temporarily override options from the .parms file. (Overkill?)
bool logging = true, verbose = false, quiet = false;
int verbose_level = 0, debug_level = 0;
bool baseoutfilename_given = false;
bool filelist = false, tap_format = false, tap_read = false;
bool tbin_file = false, do_txtfile = false, labels = true;
bool multiple_tries = false, deskew = false, adjdeskew = false, skew_given = false, add_parity = false;
bool invert_data = false, autoinvert_data = false, reverse_tape = false;
bool doing_deskew = false, doing_density_detection = false, doing_summary = false;
bool do_correction = false, find_zeros = false, do_differentiate = false;
enum flux_direction_t flux_direction_requested = FLUX_NEG, flux_direction_current = FLUX_AUTO;
bool set_ntrks_from_order = false;
bool hdr1_label = false;
bool little_endian;
byte specified_parity = 1, expected_parity = 1;
int revparity = 0;
int ww_type_to_trk[WWTRK_NUMTYPES]; // which track number each Whirlwind type is assigned to (primary/alternate clock/msb/lsb)
enum wwtrk_t ww_trk_to_type[MAXTRKS];
char *wwtracktype_names[WWTRK_NUMTYPES] = {
   "primary clk", "primary LSB", "primary MSB",
   "alternate clk", "alternate LSB", "alternate MSB" };
int head_to_trk[MAXTRKS] = { -1 };
int trk_to_head[MAXTRKS] = { -1 };
char track_order_string[MAXTRKS + 1] = { 0 };
struct tbin_hdr_t tbin_hdr = { 0 };
struct tbin_hdrext_trkorder_t tbin_hdrext_trkorder = { 0 };
struct tbin_dat_t tbin_dat = { 0 };

enum mode_t mode = PE;      // default
float bpi_specified = -1;   // -1 means not specified; 0 means do auto-detect
float ips_specified = -1;   // -1 means not specified; use tbin header or default
int ntrks_specified = -1;   // -1 means not specified; use tbin header or CSV title line
float bpi = 0, ips = 0;
int ntrks = 0, nheads = 0, samples_per_bit = 0;

enum txtfile_numtype_t txtfile_numtype = NONUM;  // suboptions for -textfile
enum txtfile_chartype_t txtfile_chartype = NOCHAR;
int txtfile_linesize = 0, txtfile_dataspace = 0;
bool txtfile_doboth;
bool txtfile_linefeed = false;
bool txtfile_verbose = true;   // (not for -tapread) (could be an option, otherwise)
char version_string[] = { VERSION };

int starting_parmset = 0;
time_t start_time;
double data_start_time = 0;
double last_block_time = 0;
double block_start_time = 0;
int skip_samples = 0, subsample = 1;
bool show_ibg = true;
int show_ibg_threshold = 5000; // by default: gaps > 5 seconds are shown
int dlog_lines = 0;

/********************************************************************
   Routines for logging and errors
*********************************************************************/
static void vlog(const char *msg, va_list args) {
   va_list args2, args3;
   va_copy(args2, args);
   va_copy(args3, args);
   vfprintf(stdout, msg, args);  // to the console
   if (logging && rlogf) vfprintf(rlogf, msg, args2); // and maybe also the log file
   if (doing_summary && summf) vfprintf(summf, msg, args3); // and maybe also the summary file
   va_end(args2); va_end(args3); }

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
         rlog("-----> debugging log stopped after %d lines\n", dlog_lines);
         endmsg_given = true; } } }

void vfatal(const char *msg, va_list args) {
   rlog("\n***FATAL ERROR: ");
   vlog(msg, args);
   rlog("\n");
   //rlog("I/O errno = %d\n", errno);
   trace_close();
   exit(99); }

void fatal(const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   vfatal(msg, args);
   va_end(args); }

void assert(bool t, const char *msg, ...) {
   if (!t) {
      va_list args;
      va_start(args, msg);
      vfatal(msg, args);
      va_end(args); } }

void open_summary_file(void) {
   if (summtxtfilename[0]) {
      assert((summf = fopen(summtxtfilename, "a")) != NULLP, "can't open summary file %s", summtxtfilename);
      doing_summary = true; } }

void close_summary_file(void) {
   if (summtxtfilename[0]) {
      fclose(summf);
      doing_summary = false; } }

/********************************************************************
   utility routines
*********************************************************************/
size_t strlcpy(char * dst, const char * src, size_t maxlen) { // from BSD library: a safe strcpy
   const size_t srclen = strlen(src);
   if (srclen + 1 < maxlen) {
      memcpy(dst, src, srclen + 1); }
   else if (maxlen != 0) {
      memcpy(dst, src, maxlen - 1);
      dst[maxlen - 1] = '\0'; }
   return srclen; }

int strcasecmp(const char*a, const char*b) {  // case-independent string comparison
   while (tolower(*a) == tolower(*b++))
      if (*a++ == '\0') return (0);
   return (tolower(*a) - tolower(*--b)); }

float scanfast_float(char **p) { // *** fast scanning routines for CSV numbers
// These routines are *way* faster than using sscanf!
   float n = 0;
   bool negative = false;
   while (**p == ' ' || **p == ',')++*p; //skip leading blanks, comma
   if (**p == '-') { // optional minus sign
      ++*p; negative = true; }
   while (isdigit(**p)) n = n * 10 + (*(*p)++ - '0'); //accumulate left of decimal point
   if (**p == '.') { // skip decimal point
      float divisor = 10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ - '0') / divisor;
         divisor *= 10; } }
   return negative ? -n : n; }

double scanfast_double(char **p) {
   double n = 0;
   bool negative = false;
   while (**p == ' ' || **p == ',')++*p;  //skip leading blanks, comma
   if (**p == '-') { // optional minus sign
      ++*p; negative = true; }
   while (isdigit(**p)) n = n * 10 + (*(*p)++ - '0'); //accumulate left of decimal point
   if (**p == '.') {
      double divisor = 10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ - '0') / divisor;
         divisor *= 10; } }
   return negative ? -n : n; }

// While we're at it: Microsoft Visual Studio C doesn't support the wonderful POSIX %' format
// specifier for nicely displaying big numbers with commas separating thousands, millions, etc.
// So here are a couple of special-purpose routines for that.
// *** BEWARE *** THEY USE A STATIC BUFFER, SO YOU CAN ONLY DO ONE CALL PER SPRINTF LINE!

char *intcommas(int n) { // 32 bits
   assert(n >= 0, "bad call to intcommas: %d", n);
   static char buf[14]; //max: 2,147,483,647
   char *p = buf + 13;  int ctr = 4;
   *p-- = '\0';
   if (n == 0)  *p-- = '0';
   else while (n > 0) {
         if (--ctr == 0) {
            *p-- = ',';  ctr = 3; }
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

char const *add_s(int value) { // make plurals have good grammar
   return value == 1 ? "" : "s"; }

void reverse2(uint16_t *pnum) {  // routines to change little- and big-endian numbers
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

/********************************************************************
   Routines for processing options
*********************************************************************/
static char *github_info = "For more information, see https://github.com/LenShustek/readtape\n";
void SayUsage (void) {
   static char *usage[] = { "",
                            "use: readtape <options> <basefilename>[.ext]", "",
                            "  The input file is <basefilename> with .csv, .tbin, or .tap,",
                            "    which may optionally be included in the command.",
                            "   If the extension is not specified, it tries .csv first",
                            "    then.tbin, and.tap only if -tapread is specified.", "",
                            "  The output files will be <basefilename>.xxx by default.", "",
                            "  The optional parameter file is <basefilename>.parms,",
                            "   or NRZI,PE,GCR,Whirlwind.parms, in the base or current directory.",
                            "",
                            "options:",
                            "  -ntrks=n       set the number of tracks to n",
                            "  -order=        set input data order for tracks 0..ntrks-2,P, where 0=MSB",
                            "                 default: 01234567P for 9 trk, 012345P for 7 trk",
                            "                 (for Whirlwind: a combination of C L M c l m and x's)",
                            "  -pe            PE (phase encoding)",
                            "  -nrzi          NRZI (non return to zero inverted)",
                            "  -gcr           GCR (group coded recording)",
                            "  -whirlwind     Whirlwind I 6-track 2-bit-per-character",
                            "  -ips=n         speed in inches/sec (default: 50, except 25 for GCR)",
                            "  -bpi=n         density in bits/inch (default: autodetect)",
                            "  -zeros         base decoding on zero crossings instead of peaks",
                            "  -differentiate do simple delta differentiation of the input data",
                            "  -even          expect even parity instead of odd (for 7-track NRZI BCD tapes)",
                            "  -revparity=n   reverse parity for blocks up to n bytes long",
                            "  -invert        invert the data so positive peaks are negative and vice versa",
                            "  -fluxdir=d     flux direction is 'pos', 'neg', or 'auto' for each block",
                            "  -reverse       reverse bits in a word and words in a block (Whirlwind only)",
                            "  -skip=n        skip the first n samples",
                            "  -blklimit=n    stop after n blocks",
                            "  -subsample=n   use only every nth data sample",
                            "  -showibg=n     report on interblock gaps greater than n milliseconds",
                            "  -tap           create one SIMH .tap file from all the data",
                            "  -deskew        do NRZI track deskewing based on the beginning data",
                            "  -skew=n,n      use this skew, in #samples for each track, rather than deducing it",
                            "  -correct       do error correction, where feasible",
                            "  -addparity     include the parity bit as the highest bit in the data (for ntrks<9)",
                            "  -tbin          only look for a .tbin input file, not .csv first",
                            "  -nolog         don't create a log file",
                            "  -nolabels      don't try to decode IBM standard tape labels",
                            "  -textfile      create an interpreted .<options>.txt file from the data",
                            "                   numeric options: -hex -octal (bytes) -octal2 (16-bit words)",
                            "                   character options: -ASCII -EBCDIC -BCD -sixbit -B5500 -SDS -SDSM",
                            "                        -flexo -adage -adagetape -CDC -Univac",
                            "                   characters per line: -linesize=nn",
                            "                   space every n bytes of data: -dataspace=n",
                            "                   make LF or CR start a new line: -linefeed",
                            "  -tapread       read a SIMH .tap file to produce a textfile; the input may have any extension",
                            "  -outf=bbb      use bbb as the <basefilename> for output files",
                            "  -outp=ppp      otherwise use ppp as an optional prepended path for output files",
                            "  -sumt=sss      append a text summary of results to text file sss",
                            "  -sumc=ccc      append a CSV summary of results to text file ccc",
                            "  -m             try multiple ways to decode a block",
                            "  -nm            don't try multiple ways to decode a block",
                            "  -v[n]          verbose mode [level n, default is 1]",
#if DEBUG
                            "  -d[n]          debug options [bits in n, default is 1]",
#endif
                            "  -q             quiet mode (only say \"ok\" or \"bad\")",
                            "  -f             take a file list from <basefilename>.txt",
                            "",
                            NULLP };
   fprintf(stderr, "readtape version %s, compiled on %s %s\n", VERSION, __DATE__, __TIME__);
   for (int i = 0; usage[i]; ++i) fprintf(stderr, "%s\n", usage[i]);
   fprintf(stderr, github_info); }

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
   int num=0, nch=0;
   if (*arg == '0') { // leading 0: alternate number base specified
      ++arg;
      if (toupper(*arg) == 'X') { // hex
         if (sscanf(++arg, "%x%n", &num, &nch) != 1) return false; }
      else if (toupper(*arg) == 'B') { // binary
         char ch; // (why is there no %b conversion?)
         while (ch = *++arg) {
            if (ch == '0') num <<= 1;
            else if (ch == '1') num = (num << 1) + 1;
            else return false; } }
      else { // octal, or just a single zero
         if (*arg != 0 && sscanf(arg, "%o%n", &num, &nch) != 1) return false; } }
   else if (sscanf(arg, "%d%n", &num, &nch) != 1) return false; // decimal
   if (num < min || num > max || arg[nch] != '\0') return false;
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
   *str = arg; // return ptr to "string" part, which could be null
   return true; }

bool opt_filename(const char* arg, const char* keyword, char* path) {
   char *str;
   if (opt_str(arg, keyword, &str)) {
      strncpy(path, str, MAXPATH); path[MAXPATH - 1] = '\0';
      return true; }
   return false; }

void assign_ww_track(int head, enum wwtrk_t tracktype) {
   assert(ww_type_to_trk[tracktype] == -1, "you already assigned track type %c", WWTRKTYPE_SYMBOLS[tracktype]);
   ww_type_to_trk[tracktype] = ntrks;
   ww_trk_to_type[ntrks] = tracktype;
   head_to_trk[head] = ntrks;
   trk_to_head[ntrks] = head;
   ++ntrks; }

bool parse_track_order(const char *str) {
   // the output are the permutations that map heads (the input data) to tracks (our processing structures)
   int temp_nheads = (int)strlen(str); // the length might be specifying the # heads
   assert(nheads <= 0 || temp_nheads == nheads, "-order length doesn't match nheads=%d", nheads);
   assert(temp_nheads >= MINTRKS && temp_nheads <= MAXTRKS, "-order can't imply ntrks=%d", temp_nheads);
   strncpy(track_order_string, str, MAXTRKS);
   if (mode == WW) { // Whirlwind examples: CMLcml..,  ..C.M.L..
      // we learn how many heads and tracks there are
      nheads = temp_nheads; // we have as many heads as the string is long
      ntrks = 0; // start counting tracks that are used
      for (int i = 0; i < WWTRK_NUMTYPES; ++i) ww_type_to_trk[i] = -1; // signal "don't have this track type"
      for (int i = 0; i < MAXTRKS; ++i) ww_trk_to_type[i] = -1; // signal "this track doesn't get used"
      for (int head = 0; head < nheads; ++head)
         switch (str[head]) {
         case 'x': head_to_trk[head] = WWHEAD_IGNORE;  break; // ignore this column's data
         case 'C': assign_ww_track(head, WWTRK_PRICLK); break; // preferred clock
         case 'L': assign_ww_track(head, WWTRK_PRILSB); break; // preferred LSB
         case 'M': assign_ww_track(head, WWTRK_PRIMSB); break; // preferred MSB
         case 'c': assign_ww_track(head, WWTRK_ALTCLK); break; // alternate clock
         case 'l': assign_ww_track(head, WWTRK_ALTLSB); break; // alternate LSB
         case 'm': assign_ww_track(head, WWTRK_ALTMSB); break; // alternate MSB
         default: fatal("bad Whirlwind track order symbol: %c in %s", str[head], str); }
      set_ntrks_from_order = true;
      assert(ww_type_to_trk[WWTRK_PRICLK] != -1, "primary clock track ('C') wasn't assigned");
      assert(ww_type_to_trk[WWTRK_PRIMSB] != -1, "primary MSB track ('M') wasn't assigned");
      assert(ww_type_to_trk[WWTRK_PRILSB] != -1, "primary LSB track ('L') wasn't assigned"); }
   else { // PE, NRZI, GCR examples: P314520, 01234567P
      int trks_done = 0; // bitmap showing which tracks are done
      for (int i = 0; i < temp_nheads; ++i) {
         byte ch = str[i];
         if (toupper(ch) == 'P') ch = (byte)temp_nheads - 1; // we put parity last
         else {
            if (!isdigit(ch)) return false; // assumes ntrks <= 11
            if ((ch -= '0') > temp_nheads - 2) return false; }
         head_to_trk[i] = ch;
         trk_to_head[ch] = i;
         trks_done |= 1 << ch; }
      if (trks_done + 1 != (1 << temp_nheads)) // must be a permutation of 0..ntrks-1
         return false;
      if (ntrks == 0) {
         ntrks = nheads = temp_nheads; // accept this as a specification of ntrks and nheads
         set_ntrks_from_order = true; } }
   return true; }

bool parse_skew(const char *arg) { // skew=1.2,4.5,0,0,1   must match ntrks
   char *str = (char *) arg;
   assert(ntrks_specified > 0, "must specify ntrks= to use skew=");
   for (int trk = 0; trk < ntrks_specified; ++trk) {
      int nch;
      if (sscanf(str, "%d%n", &skew_delaycnt[trk], &nch) != 1) fatal("bad skew at: %s", str);
      str += nch;
      skip_blanks(&str);
      if (trk < ntrks_specified - 1) {
         assert(*str++ == ',', "missing comma in skew list at: %s", str);
         skip_blanks(&str); } }
   assert(*str == 0, "extra crap in skew list: %s", str);
   return true; }

bool parse_option(char *option) { // (also called from .parm file processor)
   if (option[0] != '-') return false;
   char *arg = option + 1;
   const char *str;
   if (opt_int(arg, "NTRKS=", &ntrks_specified, MINTRKS, MAXTRKS));
   else if (opt_str(arg, "ORDER=", &str)
            && parse_track_order(str));
   else if (opt_key(arg, "NRZI")) mode = NRZI;
   else if (opt_key(arg, "PE")) mode = PE;
   else if (opt_key(arg, "GCR")) {
      mode = GCR; ips = 25; }
   else if (opt_key(arg, "WHIRLWIND")) {
      mode = WW;  bpi = 100; }
   else if (opt_key(arg, "ZEROS")) find_zeros = true;
   else if (opt_key(arg, "DIFFERENTIATE")) do_differentiate = true;
   else if (opt_flt(arg, "BPI=", &bpi_specified, 100, 10000));
   else if (opt_flt(arg, "IPS=", &ips_specified, 10, 200));
   else if (opt_int(arg, "SKIP=", &skip_samples, 0, INT_MAX));
   else if (opt_int(arg, "BLKLIMIT=", &numblks_limit, 0, INT_MAX));
   else if (opt_int(arg, "SUBSAMPLE=", &subsample, 1, INT_MAX));
   else if (opt_int(arg, "SHOWIBG=", &show_ibg_threshold, 0, INT_MAX)) show_ibg = true;
   else if (opt_int(arg, "V", &verbose_level, 0, 255)) verbose = true;
#if DEBUG
   else if (opt_int(arg, "D", &debug_level, 0, 255)) ;
#endif
   else if (opt_key(arg, "TAP")) tap_format = true;
   else if (opt_key(arg, "TAPREAD")) tap_read = true;
   else if (opt_key(arg, "EVEN")) specified_parity = expected_parity = 0;
   else if (opt_int(arg, "REVPARITY=", &revparity, 0, INT_MAX));
   else if (opt_key(arg, "INVERT")) invert_data = true;
   else if (opt_key(arg, "FLUXDIR=POS")) flux_direction_requested = FLUX_POS;
   else if (opt_key(arg, "FLUXDIR=NEG")) flux_direction_requested = FLUX_NEG;
   else if (opt_key(arg, "FLUXDIR=AUTO")) flux_direction_requested = FLUX_AUTO;
   else if (opt_key(arg, "REVERSE")) reverse_tape = true;
#if DESKEW
   else if (opt_key(arg, "DESKEW")) deskew = true;
   else if (opt_key(arg, "ADJSKEW")) adjdeskew = true;
   else if (opt_str(arg, "SKEW=", &str)
            && parse_skew(str)) deskew = skew_given = true;
#endif
   else if (opt_key(arg, "ADDPARITY")) add_parity = true;
   else if (opt_key(arg, "CORRECT")) do_correction = true;
   else if (opt_key(arg, "NOCORRECT")) do_correction = false;
   else if (opt_key(arg, "TBIN")) tbin_file = true;
   else if (opt_filename(arg, "OUTF=", baseoutfilename)) baseoutfilename_given = true;
   else if (opt_filename(arg, "OUTP=", outpathname));
   else if (opt_filename(arg, "SUMT=", summtxtfilename));
   else if (opt_filename(arg, "SUMC=", summcsvfilename));
   else if (opt_key(arg, "TEXTFILE")) do_txtfile = true;
   else if (opt_key(arg, "HEX")) txtfile_numtype = HEX;
   else if (opt_key(arg, "OCTAL2")) {
      txtfile_numtype = OCT2, txtfile_dataspace = 2; }
   else if (opt_key(arg, "OCTAL")) txtfile_numtype = OCT;
   else if (opt_key(arg, "ASCII")) txtfile_chartype = ASC;
   else if (opt_key(arg, "EBCDIC")) txtfile_chartype = EBC;
   else if (opt_key(arg, "BCD")) txtfile_chartype = BCD;
   else if (opt_key(arg, "B5500")) txtfile_chartype = BUR;
   else if (opt_key(arg, "SIXBIT")) txtfile_chartype = SIXBIT;
   else if (opt_key(arg, "SDSM")) txtfile_chartype = SDSM;
   else if (opt_key(arg, "SDS")) txtfile_chartype = SDS;
   else if (opt_key(arg, "ADAGE")) txtfile_chartype = ADAGE;
   else if (opt_key(arg, "ADAGETAPE")) txtfile_chartype = ADAGETAPE;
   else if (opt_key(arg, "FLEXO")) txtfile_chartype = FLEXO;
   else if (opt_key(arg, "CDC")) txtfile_chartype = CDC;
   else if (opt_key(arg, "UNIVAC")) txtfile_chartype = UNIVAC;
   else if (opt_int(arg, "LINESIZE=", &txtfile_linesize, 4, MAXLINE));
   else if (opt_int(arg, "DATASPACE=", &txtfile_dataspace, 0, MAXLINE));
   else if (opt_key(arg, "LINEFEED")) txtfile_linefeed = true;
   else if (opt_key(arg, "NOLOG")) logging = false;
   else if (opt_key(arg, "NOLABELS")) labels = false;
   else if (opt_key(arg, "NM")) multiple_tries = false;
   else if (option[2] == '\0') // single-character switches
      switch (toupper(option[1])) {
      case 'H':
      case '?': SayUsage(); exit(1);
      case 'M': multiple_tries = true;  break;
      case 'L': logging = true;  break;
      case 'V': verbose = true; verbose_level = 1; quiet = false; break;
#if DEBUG
      case 'D': debug_level = 1; quiet = false; break;
#endif
      case 'Q': quiet = true; verbose = false; break;
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
   block data routines
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

int count_corrected_bits (uint16_t *pdata_faked, int len) { // count how many bits we faked for all tracks
   // why did we do this instead of keeping a count as we go??
   int corrected_bits = 0;
   for (int i=0; i<len; ++i) {
      uint16_t v=pdata_faked[i];
      // This is Brian Kernighan's clever method of counting one bits in an integer
      for (; v; ++corrected_bits) v &= v-1; //clr least significant bit set
   }
   return corrected_bits; }

int count_faked_tracks(uint16_t *pdata_faked, int len) { // count how many tracks had faked bits
   // why did we do this instead of keeping a bitmask as we go?
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
   //rlog("  .tap marker %08lX, numoutbytes=%d\n", num, numoutbytes);
   for (int i = 0; i < 4; ++i) {
      byte lsb = num & 0xff;
      assert(fwrite(&lsb, 1, 1, outf) == 1, "fwrite failed in output_tap_marker");
      num >>= 8; }
   numoutbytes += 4; }

void close_file(void) {
   if (outf) {
      fclose(outf);
      if (!quiet) rlog("%s was closed at time %.8lf after %s data bytes were extracted from %d blocks\n",
                          outdatafilename, timenow, longlongcommas(numfilebytes), numfileblks);
      outf = NULLP; } }

void create_datafile(const char *name) {
   if (outf) close_file();
   if (name) { // we generated a name based on tape labels
      assert(strlen(name) < MAXPATH - 5, "create_datafile name too big 1");
      strcpy(outdatafilename, name);
      strcat(outdatafilename, ".bin"); }
   else { // otherwise create a generic name
      assert(strlen(baseoutfilename) < MAXPATH - 5, "create_datafile name too big 1");
      if (tap_format)
         sprintf(outdatafilename, "%s.tap", baseoutfilename);
      else sprintf(outdatafilename, "%s.%03d.bin", baseoutfilename, numfiles+1); }
   if (!quiet) rlog("creating file \"%s\"\n", outdatafilename);
   outf = fopen(outdatafilename, "wb");
   assert(outf != NULLP, "file create failed for \"%s\"", outdatafilename);
   ++numfiles;
   numfilebytes = numfileblks = 0;
   if (data_start_time == 0) data_start_time = timenow; }

char *modename(void) {
   return mode == PE ? "PE" : mode == NRZI ? "NRZI" : mode == GCR ? "GCR" : mode == WW ? "Whirlwind" : "???"; }

struct file_position_t {   // routines to save and restore the file position, sample time, and number of samples
   int64_t position;
   double time;
   int64_t time_ns;
   uint64_t nsamples; };

#if defined(_WIN32) // there is NO WAY to do this in an OS-independent fashion!
#define ftello _ftelli64
#define fseeko _fseeki64
#endif

void save_file_position(struct file_position_t *fp, const char *msg) {
   assert((fp->position=ftello(inf)) >= 0, "ftell failed");
   //rlog("        at %.8lf, saving position %s %s\n", timenow, longlongcommas(fp->position), msg);
   //rlog("    save_file_position %s at %.3lf msec\n", msg, timenow*1e3);
   fp->time_ns = timenow_ns;
   fp->time = timenow;
   fp->nsamples = numsamples; }

void restore_file_position(struct file_position_t *fp, const char *msg) {
   //rlog("        at %.8lf, restor position %s time %.8lf %s\n", timenow, longlongcommas(fp->position), fp->time, msg);
   assert(fseeko(inf, fp->position, SEEK_SET) == 0, "fseek failed");
   timenow_ns = fp->time_ns;
   timenow = fp->time;
   numsamples = fp->nsamples; }

static struct file_position_t blockstart;

/***********************************************************************************************
      end of block processing
***********************************************************************************************/

void show_ibg_time(void) {
   // "blockstart.time" is when we start looking for a block, so it is at the end of the previous block or tape mark.
   // "block.t_blockstart" is the time when the decoder noticed the start of data.
   int ibg_msec = (int)(((block.t_blockstart - blockstart.time) * 1000.0) + 0.5);
   //rlog("    show IBG time at %.3lf msec, started looking %.3lf msec, data started %.3lf msec\n", timenow*1e3, blockstart.time*1e3, block.t_blockstart*1e3);
   if (show_ibg_threshold == 0 || ibg_msec >= show_ibg_threshold) {
      char msg[100];
      snprintf(msg, sizeof(msg), "%.1d.%03d sec interblock gap%s\n", ibg_msec / 1000, ibg_msec % 1000,
               show_ibg_threshold > 0 ? "!" : "");
      rlog(msg);
      if (do_txtfile) txtfile_message(msg); } }

void got_tapemark(void) {
   ++numtapemarks;
   if (show_ibg) show_ibg_time();
   save_file_position(&blockstart, "after tapemark");
   //if (!quiet) rlog("  tapemark after block %d at file position %s time %.8lf\n", numfileblks, longlongcommas(blockstart.position), timenow);
   //if (!quiet) rlog("  tapemark after block %d at time %.8lf\n", numfileblks, timenow);
   if (!quiet) {
      rlog("  tapemark at time %.8lf", timenow);
      if (SHOW_TAP_OFFSET) rlog(", tap offset %lld", numoutbytes);
      if (SHOW_NUMSAMPLES) rlog(", %lld samples", numsamples);
      rlog(", %d blocks written so far\n", numblks); }
   if (do_txtfile) txtfile_tapemark(false);
   if (tap_format) {
      if (!outf) create_datafile(NULLP);
      output_tap_marker(0x00000000); }
   else if (!hdr1_label) close_file(); // not tap format: close the file if we didn't see tape labels
   hdr1_label = false; }

// format the errors and warnings that occurred in this block
char * format_block_errors(struct results_t *result) {
   // WARNING: returns pointer to static message buffer
   static char buf[MAXLINE];
   char *bufptr = buf;
   int length = result->minbits;
   if (result->errcount > 0) {
      bufptr += sprintf(bufptr, "%d err%s", result->errcount, result->errcount > 1 ? "s" : "");
      if (result->track_mismatch) bufptr += sprintf(bufptr, ", %d bit track mismatch", result->track_mismatch);
      if (result->vparity_errs) bufptr += sprintf(bufptr, ", %d parity", result->vparity_errs);
      if (result->crc_errs) bufptr += sprintf(bufptr, ", %d CRC", result->crc_errs);
      if (result->lrc_errs) bufptr += sprintf(bufptr, ", 1 LRC");
      if (result->ecc_errs) bufptr += sprintf(bufptr, ", %d ECC", result->ecc_errs);
      if (result->ww_bad_length) bufptr += sprintf(bufptr, ", bad length");
      if (result->ww_speed_err) bufptr += sprintf(bufptr, ", bad speed"); }
   else bufptr += sprintf(bufptr, "ok");
   if (result->warncount > 0) {
      bufptr += sprintf(bufptr, ", %d warning%s", result->warncount, result->warncount > 1 ? "s" : "");
      if (mode == NRZI) {
         if (result->corrected_bits > 0) {
            int trkcount; uint16_t tracks = result->faked_tracks;
            COUNTBITS(trkcount, tracks);
            bufptr += sprintf(bufptr, ", %d bits corrected on %d trks", result->corrected_bits, trkcount); } }
      if (result->gcr_bad_dgroups) bufptr += sprintf(bufptr, ", %d bad dgroups", result->gcr_bad_dgroups);
      if (result->corrected_bits > 0) bufptr += sprintf(bufptr, ", %d corrected bits", result->corrected_bits);
      if (mode == PE) {
         int corrected_bits = count_corrected_bits(data_faked, length);
         if (corrected_bits > 0) bufptr += sprintf(bufptr, ", %d faked bits on %d trks", corrected_bits, count_faked_tracks(data_faked, length)); }
      if (result->ww_leading_clock) bufptr += sprintf(bufptr, ", leading clk");
      if (result->ww_missing_onebit) bufptr += sprintf(bufptr, ", missing 1-bit");
      if (result->ww_missing_clock) bufptr += sprintf(bufptr, ", missing clk"); }
   return buf; }

void got_datablock(bool badblock) { // decoded a tape block
   struct results_t *result = &block.results[block.parmset];
   int length = result->minbits;
   if (show_ibg) show_ibg_time();
   bool labeled = !badblock && labels && ibm_label(); // process and absorb IBM tape labels
   if (length > 0 && (tap_format || !labeled)) {
      // a normal non-label data block (or in tap_format)
      if (mode != WW && length <= 2) {
         dlog("*** ignoring runt block of %d bytes at %.8lf\n", length, timenow); }
      if (badblock) {
         ++numblks_unusable;
         if (!quiet) {
            rlog("ERROR: unusable block, ");
            if (result->track_mismatch) rlog("tracks mismatched with lengths %d to %d", result->minbits, result->maxbits);
            else rlog("unknown reason");
            rlog(", %d tries, parmset %d, at time %.8lf\n", block.tries, block.parmset, timenow); } }
      else { // We have decoded a block whose data we want to write
         last_block_time = timenow;
         if (!outf) { // create a generic data file if we didn't see a file header label
            create_datafile(NULLP); }
         uint32_t errflag = result->errcount ? 0x80000000 : 0;  // SIMH .tap file format error flag
         if (tap_format) output_tap_marker(length | errflag); // leading record length
         for (int i = 0; i < length; ++i) { // discard the parity bit track and write all the data bits
            byte b = (byte)(data[i] >> 1);
            if (add_parity) b |= (data[i] & 1) << (ntrks-1);  // optionally include parity bit as the highest bit
            // maybe accumulate batches of data so we call fwrite() less often?
            assert(fwrite(&b, 1, 1, outf) == 1, "data write failed"); }
         if (tap_format) {
            byte zero = 0;  // tap format needs an even number of data bytes
            if (length & 1) {
               assert(fwrite(&zero, 1, 1, outf) == 1, "write of odd byte failed");
               numoutbytes += 1; }
            output_tap_marker(length | errflag); // trailing record length
         }
         if (do_txtfile) txtfile_outputrecord(
               block.results[block.parmset].minbits,     // length
               block.results[block.parmset].errcount,    // # errors
               block.results[block.parmset].warncount);  // # warnings
         if (mode == GCR) gcr_write_ecc_data(); // TEMP for GCR

         //if (DEBUG && (result->errcount != 0 || result->corrected_bits != 0))
         //   show_block_errs(result->maxbits);
         //if (DEBUG && mode == NRZI) {
         //   if (result->crc_errs) dlog("bad CRC: %03x\n", result->crc);
         //   if (result->lrc_errs) dlog("bad LRC: %03x\n", result->lrc); }

         // Here's a summary of what kind of block errors and warnings we record,
         // and whether there could be an arbitrary number of N instances within
         // a block, or only the specified numbers are possible.
         //
         //                                    9trk    7trk
         //                              PE    NRZI    NRZI     GCR    WW
         // err: tracks mismatched       N      N        N       N
         // err: vertical parity         N      N        N       N
         // err: CRC bad                ...    0,1      ...    0,1,2
         // err: LRC bad                ...    0,1      0,1     ...
         // err: ECC bad                ...    ...      ...      N
         // err: sgroup sequence bad    ...    ...      ...      N
         // err: bad block length                                      0,1
         // warn: peak after midbit     ...     N        N      ...
         // warn: bits were corrected    N      N        N      ...
         // warn: dgroup bad             ...    ...      ...      N
         // warn: extra leading clock                                  0,1
         //
         // "errcount" is the sum of the first 6, which are serious errors
         // "warncount" is the sum of the last 3, which are less serious

         if (result->errcount != 0) ++numblks_err;
         if (result->warncount != 0) ++numblks_warn;
         if (verbose || (numblks == 0)
               || (!quiet && (result->errcount > 0 || result->warncount > 0 || badblock))) {
            rlog("wrote block %3d, %4d bytes, %d %s, parmset %d, ",
                 numblks + 1, length, block.tries, block.tries > 1 ? "tries" : "try", block.parmset);
            if (result->alltrk_min_agc_gain == FLT_MAX)
               rlog("max AGC %.2f, ", result->alltrk_max_agc_gain);
            else rlog("AGC %.2f-%.2f, ", result->alltrk_min_agc_gain, result->alltrk_max_agc_gain);
            rlog(format_block_errors(result)); // print info about the errors and warnings
            rlog(", avg speed %.2f IPS at time %.8lf", 1 / (result->avg_bit_spacing * bpi), timenow);
            if (SHOW_START_TIME) rlog(", start %.8lf", block.t_blockstart);
            if (SHOW_TAP_OFFSET) rlog(", tap offset %lld", numoutbytes);
            if (SHOW_NUMSAMPLES) rlog(", %lld samples", numsamples);
            rlog("\n");

            if (!verbose && numblks == 0) rlog("(subsequent good blocks will not be shown because -v wasn't specified)\n"); }
         if (result->track_mismatch) {
            //rlog("   WARNING: tracks mismatched, with lengths %d to %d\n", result->minbits, result->maxbits);
            ++numblks_trksmismatched; }
         if (result->missed_midbits > 0) {
            ++numblks_midbiterrs;
            rlog("   WARNING: %d bits were before the midbit using parmset %d for block %d at %.8lf\n",
                 result->missed_midbits, block.parmset, numblks + 1, timenow); //
         }
         if (result->corrected_bits > 0) ++numblks_corrected;
         numfilebytes += length;
         numoutbytes += length;
         numdatabytes += length;
         ++numfileblks;
         ++numblks; } }
   if (adjdeskew && mode == NRZI) adjust_deskew(nrzi.clkavg.t_bitspaceavg);
   save_file_position(&blockstart, "after block done"); // remember the file position for the start of the next block
//log("got valid block %d, file pos %s at %.8lf\n", numblks, longlongcommas(blockstart.position), timenow);
};

/*****************************************************************************************
      tape data processing, in either CSV (ASCII) or TBIN (binary) format
******************************************************************************************/

void read_tbin_header(void) {  // read the .TBIN file header
   if (!quiet) rlog("\n.tbin file header:\n");
   // (1) read the fixed header part
   assert(fread(&tbin_hdr, sizeof(tbin_hdr), 1, inf) == 1, "can't read .tbin header");
   assert(strcmp(tbin_hdr.tag, HDR_TAG) == 0, ".tbin file missing "HDR_TAG" tag");
   if (!little_endian)  // convert all 4-byte integers in the header to big-endian
      for (int i = 0; i < sizeof(tbin_hdr.u.s) / 4; ++i)
         reverse4(&tbin_hdr.u.a[i]);
   assert(tbin_hdr.u.s.format == TBIN_FILE_FORMAT, "bad .tbin file header version");
   assert(tbin_hdr.u.s.tbinhdrsize == sizeof(tbin_hdr),
          "bad .tbin hdr size: %d, not %d", tbin_hdr.u.s.tbinhdrsize, sizeof(tbin_hdr));
   if (tbin_hdr.u.s.ntrks != 0) {
      if (ntrks <= 0) {
         ntrks = nheads = tbin_hdr.u.s.ntrks;
         if (!quiet) rlog("  using .tbin ntrks = %d\n", ntrks); }
      else if (tbin_hdr.u.s.ntrks != ntrks) ("*** WARNING *** .tbin file says %d trks but ntrks=%d\n", tbin_hdr.u.s.ntrks, ntrks); }
   if (tbin_hdr.u.s.mode != UNKNOWN) {
      mode = tbin_hdr.u.s.mode;
      if (!quiet) rlog("  using .tbin mode = %s\n", modename()); }
   if (bpi_specified < 0 && tbin_hdr.u.s.bpi != 0) {
      bpi = tbin_hdr.u.s.bpi;
      if (!quiet) rlog("  using .tbin bpi = %.0f\n", bpi); }
   if (ips_specified < 0 && tbin_hdr.u.s.ips != 0) {
      ips = tbin_hdr.u.s.ips;
      if (!quiet) rlog("  using .tbin ips = %.0f\n", ips); }
   sample_deltat_ns = tbin_hdr.u.s.tdelta;
   sample_deltat = (float)sample_deltat_ns / 1e9f;
   // (2) read the optional "trkorder" header extension
   if (tbin_hdr.u.s.flags & TBIN_TRKORDER_INCLUDED) {
      assert(fread(&tbin_hdrext_trkorder, sizeof(tbin_hdrext_trkorder), 1, inf) == 1, "can't read .tbin trkorder header extension");
      assert(strcmp(tbin_hdrext_trkorder.tag, HDR_TRKORDER_TAG) == 0, ".tbin file missing "HDR_TRKORDER_TAG" tag");
      if (track_order_string[0] && strcmp(tbin_hdrext_trkorder.trkorder, track_order_string) != 0) {
         if (!quiet) rlog("  the .tbin head order %s is being ignored because it was specified as %s on the command line\n",
                             tbin_hdrext_trkorder.trkorder, track_order_string); }
      else {
         assert(parse_track_order(tbin_hdrext_trkorder.trkorder), "invalid track order in TBIN file: %s", tbin_hdrext_trkorder.trkorder);
         if (!quiet) rlog("  -order=%s\n", tbin_hdrext_trkorder.trkorder); } }
   if (!quiet) {
      if (!(tbin_hdr.u.s.flags & TBIN_NO_REORDER)) {  // track reordering has been done
         rlog("  ");
         if (head_to_trk[0] != -1)
            rlog("-order was ignored because ");
         rlog ("the track ordering was changed to the canonical order when the .tbin file was created\n"); }
      if (tbin_hdr.u.s.flags & TBIN_INVERTED) rlog("  the waveforms were inverted by CSVTBIN\n");
      if (tbin_hdr.u.s.flags & TBIN_REVERSED) rlog("  the tape may have been read or written backwards\n");
      if (tbin_hdr.descr[0] != 0) rlog("   description: %s\n", tbin_hdr.descr);
      if (tbin_hdr.u.s.time_written.tm_year > 0)   rlog("  created on:   %s", asctime(&tbin_hdr.u.s.time_written));
      if (tbin_hdr.u.s.time_read.tm_year > 0)      rlog("  read on:      %s", asctime(&tbin_hdr.u.s.time_read));
      if (tbin_hdr.u.s.time_converted.tm_year > 0) rlog("  converted on: %s", asctime(&tbin_hdr.u.s.time_converted));
      rlog("  max voltage: %.1fV\n", tbin_hdr.u.s.maxvolts);
      rlog("  time between samples: %.3f usec\n", (float)tbin_hdr.u.s.tdelta / 1000); }
   // (3) read the data section header
   assert(fread(&tbin_dat, sizeof(tbin_dat), 1, inf) == 1, "can't read .tbin dat");
   assert(strcmp(tbin_dat.tag, DAT_TAG) == 0, ".tbin file missing DAT tag");
   assert(tbin_dat.sample_bits == 16, "we support only 16 bits/sample, not %d", tbin_dat.sample_bits);
   if (!little_endian) reverse8(&tbin_dat.tstart); // convert to big endian if necessary
   timenow_ns = tbin_dat.tstart;
   timenow = (float)timenow_ns / 1e9; };

void force_end_of_block(void) {
   if (mode == PE) pe_end_of_block();
   else if (mode == NRZI && nrzi.datablock) nrzi_end_of_block();
   else if (mode == GCR) gcr_end_of_block(); }

void differentiate(struct sample_t *psample, int trk) { // do simple-minded differentiation: delta between successive voltage samples
   float voltage = psample->voltage[trk];
   float delta = voltage - trkstate[trk].v_last_raw;
   if (delta < DIFFERENTIATE_THRESHOLD && delta > -DIFFERENTIATE_THRESHOLD) delta = 0;
   trkstate[trk].v_last_raw = voltage;
   psample->voltage[trk] = delta * DIFFERENTIATE_SCALE * samples_per_bit;
   static int derivative_dumps = 0;
   if (0 && trk == TRACETRK) {
      if (derivative_dumps == 0 && timenow >= 0.8608976) ++derivative_dumps; // start dumping
      if (derivative_dumps > 0 && derivative_dumps < 2000) {
         rlog("derivative, %.8f, %.4f, %.4f\n", timenow, voltage, psample->voltage[trk]);
         ++derivative_dumps; } } }

bool readblock(bool retry) { // read the CSV or TBIN file until we get to the end of a tape block
   // return false if we are at the endfile
   struct sample_t sample;
   bool did_processing = false;
   bool endfile = false;
   enum bstate_t blockkind;

   samples_per_bit = bpi > 0 ? (int)(1 / (bpi*ips*sample_deltat)) : 20;
   do { // loop reading samples
      if (!retry) ++lines_in;
      if (tbin_file) { // TBIN file
         int16_t tbin_voltages[MAXTRKS];
         for (int i = 0; i < subsample; ++i) { // read the subsample=nth line and ignore all the others
            assert(fread(&tbin_voltages[0], 2, 1, inf) == 1, "can't read .tbin data for head 0 at time %.8lf", timenow);
            if (!little_endian) reverse2((uint16_t *)&tbin_voltages[0]);
            if (tbin_voltages[0] == -32768 /*0x8000*/) { // end of file marker
               if (did_processing) force_end_of_block(); // force "end of block" processing
               endfile = true;
               goto done; }
            assert(fread(&tbin_voltages[1], 2, nheads - 1, inf) == nheads - 1, "can't read .tbin data for heads 1.. at time %.8lf", timenow); }
         if (!little_endian)
            for (int head = 1; head < nheads; ++head)
               reverse2((uint16_t *)&tbin_voltages[head]);
         for (int head = 0; head < nheads; ++head) {
            int trk = head_to_trk[head];
            sample.voltage[trk] = (float)tbin_voltages[head] / 32767 * tbin_hdr.u.s.maxvolts;
            if (invert_data) sample.voltage[trk] = -sample.voltage[trk];
            if (do_differentiate) differentiate(&sample, trk); }
         sample.time = (double)timenow_ns / 1e9;
         timenow_ns += sample_deltat_ns; // (for next time)
      }
      else {  // CSV file
         char line[MAXLINE + 1];
         for (int i=0; i < subsample; ++i) // read the subsample=nth line and ignore all the others
            if (!fgets(line, MAXLINE, inf)) {
               if (did_processing) force_end_of_block(); // force "end of block" processing
               endfile = true;
               goto done; }
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
         for (int head = 0; head < nheads; ++head) { // read voltages for all tracks, and permute as requested
            int trk = head_to_trk[head];
            sample.voltage[trk] = scanfast_float(&linep);
            if (invert_data) sample.voltage[trk] = -sample.voltage[trk];
            if (do_differentiate) differentiate(&sample, trk); } }
      ++numsamples;
      timenow = sample.time;
      if (torigin == 0) torigin = timenow; // for debugging output

      if (!block.window_set) { //
         // set the width of the peak-detect moving window
         if (bpi)
            pkww_width = min(PKWW_MAX_WIDTH, (int)(PARM.pkww_bitfrac / (bpi*ips*sample_deltat)));
         else pkww_width = 8; // a random reasonable choice if we don't have BPI specified
         static bool said_rates = false;
         if (!quiet && !said_rates) {
            rlog("\nexecution-time configuration:\n");
            if (set_ntrks_from_order) rlog("  we set ntrks=%d as implied by the -order string \"%s\"\n", ntrks, track_order_string);
            rlog("  %d track %s encoding, %s parity, %d BPI at %d IPS", ntrks, modename(),
                 mode == WW ? "no" : expected_parity ? "odd" : "even", (int)bpi, (int)ips);
            if (bpi != 0) rlog(" (%.2f usec/bit)", 1e6f / (bpi*ips));
            rlog("\n  first sample is at time %.8lf seconds on the tape\n", timenow);
            if (subsample > 1) rlog("  subsampling every %d samples\n", subsample);
            if (invert_data) rlog("  inverting the data polarity\n");
            if (reverse_tape) rlog("  reversing the bit pairs in each word, and the words in each block\n");
            rlog("  sampling rate is %s Hz (%.2f usec)",
                 intcommas((int)(1.0 / sample_deltat)), sample_deltat*1e6);
            if (bpi != 0) rlog(", or about %d samples per bit", (int)(1/(bpi*ips*sample_deltat)));
            rlog("\n");
            if (bpi != 0 && (int)(1 / (bpi*ips*sample_deltat)) > 100)
               rlog("  ---> Warning: excessive samples per bit; consider using the -subsample option\n");
            if (find_zeros) rlog("  will look for zero crossings, not peaks\n");
            else rlog("  peak detection window width is %d samples (%.2f usec)\n", pkww_width, pkww_width * sample_deltat*1e6);
            if (mode == WW) {
               rlog("  Whirlwind data has %d tracks from %d data heads assigned as follows:\n", ntrks, nheads);
               for (int tracktype = 0; tracktype < WWTRK_NUMTYPES; ++tracktype) {
                  int trk = ww_type_to_trk[tracktype];
                  if (trk == -1) rlog("              there is no  ");
                  else /*     */ rlog("    track %d, head %d is the ", trk, trk_to_head[trk]);
                  rlog(" %s, '%c'\n", wwtracktype_names[tracktype], WWTRKTYPE_SYMBOLS[tracktype]); }
               for (int head = 0; head < nheads; ++head)
                  if (head_to_trk[head] == WWHEAD_IGNORE) rlog("             head %d is unused\n", head);
               rlog("  the initial peak polarity for each flux change %s\n",
                    flux_direction_requested == FLUX_AUTO ? "will be automatically determined for each block"
                    : flux_direction_requested == FLUX_POS ? "is expected to be positive"
                    : flux_direction_requested == FLUX_NEG ? "is expected to be negative"
                    : "--- internal error  ---"); }
            else {
               rlog("  input data order: ");
               for (int i = 0; i < ntrks; ++i) {
                  if (head_to_trk[i] == ntrks - 1) rlog("p");
                  else rlog("%d", head_to_trk[i]);
                  if (head_to_trk[i] == 0) rlog("(msb)");
                  if (head_to_trk[i] == ntrks - 2) rlog("(lsb)"); }
               rlog("\n"); }
            rlog("\n");
            said_rates = true; }
         block.window_set = true; }

      did_processing = true;
      blockkind = process_sample(&sample);  // process this sample
   }
   while (blockkind == BS_NONE);        // until we get a block

done:;
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   result->errcount = // sum up all the possible errors for all types
      result->track_mismatch + result->vparity_errs + result->ecc_errs + result->crc_errs + result->lrc_errs
      + result->gcr_bad_sequence + result->ww_bad_length + result->ww_speed_err;
   result->warncount = // sum up all the possible warnings for all types
      result->missed_midbits + result->corrected_bits + result->gcr_bad_dgroups
      + result->ww_leading_clock + result->ww_missing_onebit + result->ww_missing_clock;
   return !endfile; //
} // readblock

/***********************************************************************************************
   file processing
***********************************************************************************************/
bool rereading;

static char *bs_names[] = // must agree with enum bstate_t in decoder.h
{ "BS_NONE", "BS_TAPEMARK", "BS_NOISE", "BS_BADBLOCK", "BS_BLOCK", "ABORTED" };

void show_program_info(int argc, char *argv[]) {
   char line[MAXLINE + 1];
   rlog("this is readtape version %s, compiled on %s %s, running on %s",
        VERSION, __DATE__, __TIME__, ctime(&start_time)); // ctime ends with newline!
   if (DEBUG) rlog("**** DEBUG version %s\n", TRACEFILE ? " with tracing" : "");
#if defined(_WIN32)
   {
      char *pgmpathptr;
      rlog("  executable file: ");
      if (_get_pgmptr(&pgmpathptr) == 0) rlog("%s\n", pgmpathptr);
      else rlog("<unavailable>\n"); }
#endif
   rlog("  command line: ");
   for (int i = 0; i < argc; ++i)  // for documentation, show invocation options
      rlog("%s ", argv[i]);
   rlog("\n");
#if defined(_WIN32)
   rlog("  current directory: %s\n", _getcwd(line, MAXLINE));
#endif
   rlog("  this is a %s-endian computer\n", little_endian ? "little" : "big");
   rlog("  %s", github_info); }


void show_decode_status(void) { // show the results of all the decoding tries
   rlog("parmset decode status:\n");
   for (int i = 0; i < MAXPARMSETS; ++i) {
      struct results_t *result = &block.results[i];
      if (result->blktype != BS_NONE)
         rlog(" parmset %d result was %12s with %d errors, %d warnings, length min %d max %d, avg bitspace %.3f\n",
              i, bs_names[result->blktype], result->errcount, result->warncount,
              result->minbits, result->maxbits, result->avg_bit_spacing); } }

//*** process a complete input file whose path and base file name are in baseinfilename[]
//*** return TRUE only if all blocks were well-formed and error-free

bool process_file(int argc, char *argv[], const char *extension) {
   char logfilename[MAXPATH];
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

   indatafilename[MAXPATH - 5] = '\0';
   inf = NULL;
   if (!tbin_file && strcasecmp(extension, ".tbin") != 0) {
      strncpy(indatafilename, baseinfilename, MAXPATH - 5);  // try to open <baseinfilename>.csv
      strcat(indatafilename, ".csv");
      inf = fopen(indatafilename, "r");
      tbin_file = false; }
   if (!inf) {
      strncpy(indatafilename, baseinfilename, MAXPATH - 6);  // try to open <baseinfilename>.tbin
      strcat(indatafilename, ".tbin");
      inf = fopen(indatafilename, "rb");
      assert(inf != NULL, "Unable to open input file \"%s\" .tbin or .csv", baseinfilename);
      tbin_file = true; }
   if (!quiet) {
      show_program_info(argc, argv);
      rlog("\nreading file \"%s\"\n", indatafilename);
      rlog("the output files will be \"%s.xxx\"\n", baseoutfilename); }
   if (tbin_file) read_tbin_header();  // sets NRZI, etc., so do before read_parms()

   read_parms(); // read the .parm file, if any
   if (ntrks_specified > 0) {
      if (ntrks == 0) ntrks = nheads = ntrks_specified;
      else assert(ntrks == ntrks_specified, "ntrks=%d doesn't match what we already deduced: %d", ntrks_specified, ntrks); }

   if (!tbin_file) {
      // first two (why?) lines in the input file are headers from Saleae
      assert(fgets(line, MAXLINE, inf) != NULLP, "Can't read first CSV title line");
      assert(fgets(line, MAXLINE, inf) != NULLP, "Can't read second CSV title line");
      unsigned int numcommas = 0;
      for (int i = 0; line[i]; ++i) if (line[i] == ',') ++numcommas;
      if (ntrks <= 0) {
         ntrks = nheads = numcommas;
         rlog("  derived ntrks=%d from .CSV file header\n", ntrks); }
      else if (numcommas != nheads) rlog("*** WARNING *** input file has %d columns of data, but ntrks=%d\n", numcommas, ntrks);
      // For CSV files, set sample_deltat by reading the first 10,000 samples, because Saleae timestamps
      // are only given to 0.1 usec. If the sample rate is, say, 3.125 Mhz, or 0.32 usec between samples,
      // then all the timestamps are off by either +0.08 usec or -0.02 usec!
      // Note that we have to adjust for the subsampling we might be doing later.
      struct file_position_t filestart;
      save_file_position(&filestart, "at start of file before computing delta"); // remember where we are starting
      int linecounter = 0;
      double first_timestamp = -1;
      while (fgets(line, MAXLINE, inf) && ++linecounter < 10000) {
         char *linep = line;
         double timestamp = scanfast_double(&linep);
         if (first_timestamp < 0) first_timestamp = filestart.time = timenow = timestamp;
         else sample_deltat = (float)((timestamp - first_timestamp)*subsample/(linecounter-1)); }
      restore_file_position(&filestart, "at start of file after computing delta"); // go back to reading the first sample
      //rlog("sample_delta set to %.2f usec after %s samples\n", sample_deltat*1e6, intcommas(linecounter));
   }
   if (skip_samples > 0) {
      if (!quiet) rlog("skipping the first %s samples...\n", intcommas(skip_samples));
      while (skip_samples--) {
         bool endfile;
         struct sample_t sample;
         if (tbin_file) endfile = fread(sample.voltage, 2, ntrks, inf) != ntrks;
         else endfile = !fgets(line, MAXLINE, inf);
         assert(!endfile, "endfile with %d lines left to skip\n", skip_samples); } }
   interblock_counter = 0;
   starting_parmset = 0;

   assert(!add_parity || ntrks < 9, "-parity not allowed with ntrks=%d", ntrks);
   if (head_to_trk[0] == -1 // if no input track permutation was given
         || (tbin_file && !(tbin_hdr.u.s.flags & TBIN_NO_REORDER))) // or the tbin file had a permutation applied to it
      for (int i = 0; i < ntrks; ++i) head_to_trk[i] = trk_to_head[i] = i; // create default
   if (ips_specified >= 0) ips = ips_specified; // command line overrides tbin header
   if (ips == 0) ips = 50;  // default IPS
   if (bpi_specified >= 0) bpi = bpi_specified;  // command line overrides tbin header
   if (mode == GCR) {
      if (bpi != 9042) rlog ("BPI was reset to 9042 for GCR 6250\n");
      bpi = 9042; } // the real BPI isn't 6250!

   if (bpi == 0) {  // **** auto-detect the density by looking at how close transitions are at the start of the tape
      doing_density_detection = true;
      estden_init();
      int nblks = 0;
      struct file_position_t filestart;
      save_file_position(&filestart, "at start of file before computing bpi"); // remember the file position for the start of the file
      do {
         init_blockstate(); // do one block
         block.parmset = starting_parmset;
         init_trackstate();
         if (!readblock(true)) break; // stop if endfile
         if(block.results[block.parmset].blktype != BS_NOISE) ++nblks; }
      while (!estden_done()); // keep going until we have enough transitions
      estden_setdensity(nblks);
      restore_file_position(&filestart, "at start of file after computing bpi");
      interblock_counter = 0;
      doing_density_detection = false; }

   if (mode == WW) init_trackstate(); // for Whirlwind, initialize track state only once because block can be very close together

#if DESKEW     // ***** automatic deskew determination based on the first few blocks, or values specified
   if (deskew) {
      if (mode == PE) rlog("-deskew option is ignored for PE\n");
      else { // currently only for NRZI, GCR, and WW
         if (skew_given) {
            if (!quiet) skew_display(); }
         else {
            doing_deskew = true;
            if (!quiet) rlog("\nstarting preprocessing to determine head skew...\n");
            int nblks = 0;
            struct file_position_t filestart;
            save_file_position(&filestart, "at start of file before computing skew"); // remember the file position for the start of the file
            int min_transitions = 0;
            do { // do one block at a time
               init_blockstate();
               block.parmset = starting_parmset;
               if (mode == WW) ww_init_blockstate();  // for Whirlwind, we initialize the whole track state only once because blocks can be very close together
               else init_trackstate();
               if (!readblock(true)) break; // stop if endfile
               if (block.results[block.parmset].blktype != BS_NOISE) {
                  min_transitions = skew_min_transitions();
                  ++nblks; } } // keep going until we have enough transitions or have processed too many blocks
            while (nblks < MAXSKEWBLKS && min_transitions < MINSKEWTRANS);
            assert(min_transitions > 0, "Some tracks have no transitions. Is ntrks=%d correct?", ntrks);
            if (!quiet) rlog("head skew compensation after reading the first %d blocks:\n", nblks);
            skew_compute_deskew(true);
            restore_file_position(&filestart, "at start of file after computing skew");
            interblock_counter = 0;
            output_peakstats("_deskew");
            rlog("\n");
            if (mode == WW) { // for Whirlwind we compute average peak height during deskew, because we have no other opportunity
               init_trackpeak_state(); // erase skew and peak state
               ww.t_lastblockmark = 0;
               ww.blockmark_queued = false;
               for (int trk = 0; trk < ntrks; ++trk) {
                  struct trkstate_t *t = &trkstate[trk];
                  int count = t->v_avg_height_count;
                  compute_avg_height(t);
                  rlog("  trk %d average peak height is %.2fV and AGC is %.2f, based on %d measurements\n",
                       trk, t->v_avg_height / 2, t->agc_gain, count); }
               rlog("\n"); }
            doing_deskew = false; } } }
#endif
   bool endfile = false;
   while (!endfile && numblks < numblks_limit) { // keep processing lines of the file for more blocks
      init_blockstate();  // initialize for first processing of a new block
      block.parmset = starting_parmset;
      save_file_position(&blockstart, "to remember block start"); // remember the file position for the start of a block
      dlog("\n*** start block search at file pos %s at %.8lf\n", longlongcommas(blockstart.position), timenow);

      bool keep_trying;
      int last_parmset;
      block.tries = 0;

#if GCR_PARMSCAN // Scan for optimal sets of GCR parms for the first block
      if (numblks == 0) {
         int clk_window = 0; //for (int clk_window = 10; clk_window <= 30; clk_window += 5)
         for (float clk_alpha = 0.010f; clk_alpha <= 0.030f; clk_alpha += 0.002f)
            for (float pulse_adj = 0.2f; pulse_adj <= 0.401f; pulse_adj += 0.1f)
               for (float z1pt = 1.4f; z1pt <= 1.501f; z1pt += .01f)
                  for (float z2pt = 2.20f; z2pt <= 2.501f; z2pt += .02f) {
                     PARM.clk_window = clk_window;
                     PARM.clk_alpha = clk_alpha;
                     PARM.pulse_adj = pulse_adj;
                     PARM.z1pt = z1pt;
                     PARM.z2pt = z2pt;
                     init_trackstate();
                     readblock(true);
                     //rlog("with clk_window %d pulseadj %.3f z1pt %.3f z2pt %.3f firsterr %d\n",
                     //   clk_window, pulse_adj, z1pt, z2pt, block.results[block.parmset].first_error);
                     rlog("clk_alpha %.3f pulseadj %.3f z1pt %.3f z2pt %.3f firsterr %4d ",
                          clk_alpha, pulse_adj, z1pt, z2pt, block.results[block.parmset].first_error);
                     rlog("errors %d warnings %d minbits %d maxbits %d\n",
                          block.results[block.parmset].errcount, block.results[block.parmset].warncount,
                          block.results[block.parmset].minbits, block.results[block.parmset].maxbits);
                     restore_file_position(&blockstart, "restart block for GCR scan"); // go back to try again
                     interblock_counter = 0; } }
      // copy and paste the log lines into Excel using the Text Import Wizard, then sort as desired
#endif
      do { // keep reinterpreting a block with different parameters until we get a perfect block or we run out of parameter choices
         keep_trying = false;
         last_parmset = block.parmset;
         //window_set = false;
         if (mode == WW) ww_init_blockstate();  // for Whirlwind, we initialize the whole track state only once because blocks can be very close together
         else init_trackstate();
         if (verbose_level & VL_ATTEMPTS) rlog("     trying block %d with parmset %d at byte %s, time %.8lf\n", numblks + 1, block.parmset, longlongcommas(blockstart.position), timenow);
         if (mode == WW && ww.blockmark_queued) {
            ww_blockmark(); // returned the queued-up blockmark from the end of the last block
            block.t_blockstart = timenow - ww.clkavg.t_bitspaceavg; // and say that it started one bit ago
         }
         else endfile = !readblock(block.tries > 0); // ***** read a block ******
         struct results_t *result = &block.results[block.parmset];
         if (result->blktype == BS_NONE) goto endfile; // stuff at the end wasn't a real block
         ++block.tries;
         ++PARM.tried;  // note that we used this parameter set in another attempt
         if (verbose_level & VL_ATTEMPTS) rlog("       block %d is type %s with parmset %d; minlength %d, maxlength %d, %d errors, %d warnings, %d corrected bits at %.8lf\n", //
                                                  numblks + 1, bs_names[result->blktype], block.parmset, result->minbits, result->maxbits, result->errcount, result->warncount, result->corrected_bits, timenow);
         if (result->blktype == BS_TAPEMARK) goto done;  // if we got a tapemake, we're done
         if (result->blktype == BS_NOISE && SKIP_NOISE) goto done; // if we got noise and are immediately skipping noise blocks, we're done
         if (result->blktype == BS_BLOCK && result->errcount == 0 && result->warncount == 0) { // if we got a perfect block, we're done
            if (block.tries>1) ++numblks_goodmultiple;  // bragging rights; perfect blocks due to multiple parameter sets
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
               restore_file_position(&blockstart, "to retry the same block");
               interblock_counter = 0;
               dlog("   retrying block %d with parmset %d at byte %s at time %.8lf\n", numblks + 1, block.parmset, longlongcommas(blockstart.position), timenow); } } }
      while (keep_trying);

      // We didn't succeed in getting a perfect decoding of the block, so pick the best of all our bad decodings.

      if (block.tries == 1) { // unless we don't have multiple decoding tries
         if (block.results[block.parmset].errcount > 0) ok = false; }
      else {

         dlog("looking a block without errors and the minimum warning\n");
         int min_warnings = INT_MAX;
         for (int i = 0; i < MAXPARMSETS; ++i) { // Try 1: find a decoding with no errors and the minimum number of warnings
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->errcount == 0 && result->warncount < min_warnings) {
               min_warnings = result->warncount;
               block.parmset = i; } }
         if (min_warnings < INT_MAX) {
            dlog("  best no-error choice is parmset %d\n", block.parmset);
            goto done; }
         ok = false; // we had at least one bad block

         dlog("looking for an ok block with the minimum errors\n");
         int min_errors = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 2: Find the decoding with the mininum number of errors
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->errcount < min_errors) {
               min_errors = result->errcount;
               block.parmset = i; } }
         if (min_errors < INT_MAX) {
            dlog("  best error choice is parmset %d\n", block.parmset);
            goto done; }

         dlog("looking for the bad block with the minimum track difference\n");
         int min_track_diff = INT_MAX;
         for (int i = 0; i < MAXPARMSETS; ++i) { // Try 3: Find the decoding with the minimum difference in track lengths
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BADBLOCK && result->track_mismatch < min_track_diff) {
               min_track_diff = result->track_mismatch;
               block.parmset = i; } }
         if (min_track_diff < INT_MAX) {
            dlog("  best bad block choice is parmset %d with mismatch %d\n",  block.parmset, block.results[block.parmset].track_mismatch);
            goto done; }

         dlog("looking for what must be a noise block\n");
         for (int i = 0; i < MAXPARMSETS; ++i) { // Try 4: Find the first decoding which is a noise block
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_NOISE) {
               block.parmset = i;
               dlog("  best block is parmset %d, which is noise\n", block.parmset);
               goto done; } }
         assert(false, "block state error in process_file()\n"); }

done:;
      struct results_t *result = &block.results[block.parmset];
      if (DEBUG && block.tries > 1) show_decode_status();
      if (multiple_tries) dlog("  chose parmset %d as best after %d tries, type %s\n", block.parmset, block.tries, bs_names[result->blktype]);

      if (result->blktype != BS_NOISE) {
         ++PARM.chosen;  // count times that this parmset was chosen to be used

         if (block.tries > 1 // if we processed the block multiple times
               && last_parmset != block.parmset) { // and the decoding we chose isn't the last one we did
            restore_file_position(&blockstart, "to recompute the best decoding");    // then reprocess the chosen one to recompute that best data
            interblock_counter = 0;
            dlog("     rereading block %d with parmset %d at byte %s at time %.8lf\n", numblks + 1, block.parmset, longlongcommas(blockstart.position), timenow);
            rereading = true;
            init_trackstate();
            endfile = !readblock(true);
            rereading = false;
            dlog("     reread of block %d with parmset %d is type %s, minlength %d, maxlength %d, %d errors, %d corrected bits at %.8lf\n", //
                 numblks + 1, block.parmset, bs_names[result->blktype], result->minbits, result->maxbits, result->errcount, result->corrected_bits, timenow); }

         switch (block.results[block.parmset].blktype) {  // process the block according to our best decoding
         case BS_TAPEMARK:
            got_tapemark(); break;
         case BS_BLOCK:
            got_datablock(false); break;
         case BS_BADBLOCK:
            got_datablock(true); break;
         default:
            fatal("bad block state after decoding", ""); //
         } }
#if USE_ALL_PARMSETS
      do { // If we start with a new parmset each time, we'll use them all relatively equally and can see which ones are best
         if (++starting_parmset >= MAXPARMSETS) starting_parmset = 0; }
      while (parmsetsptr[starting_parmset].clk_factor == 0);
#else
// otherwise we always start with parmset 0, which is configured to be the best for most tapes
#endif
   }  // next line of the file
endfile:
   if (numblks >= numblks_limit) rlog("\n***blklimit=%d reached\n", numblks_limit);
   if (tap_format && outf) output_tap_marker(0xffffffffl);
   if (do_txtfile) txtfile_close();
   close_file();
   trace_close();
   return ok; }

void breakpoint(void) { // for the debugger
   static int counter;
   ++counter; }

/**********************************************************************
   main
**********************************************************************/

//void test(const char *str) {
//   int x;
//   bool ans = opt_int(str, "V=", &x, 0, 255);
//   printf("%s: %d, %d\n", str, ans, x); }

int main(int argc, char *argv[]) {
   int argno;
   char cmdfilename[MAXPATH];
   char cmdfileext[15];

#if 0 // test new opt_int
   test("v=123");
   test("v=0x12");
   test("v=0b1010");
   test("v=0b1111 ");
   test("v=0b");
   test("v=021");
   test("v=03");
   test("v=010");
   exit(0);
#endif

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
      SayUsage(); exit(4); }
   argno = HandleOptions(argc, argv);
   if (txtfile_numtype != NONUM || txtfile_chartype != NOCHAR)
      do_txtfile = true; // assume -txtfile if any of its suboptions were given
   if (do_txtfile) {
      //if (txtfile_chartype == NOCHAR && txtfile_numtype == NONUM) {
      //   txtfile_chartype = ASC; txtfile_numtype = HEX; }
      txtfile_doboth = txtfile_chartype != NOCHAR && txtfile_numtype != NONUM;
      if (txtfile_linesize == 0) txtfile_linesize = txtfile_doboth ? 32 : 64; }

   if (argno == 0) {
      fprintf(stderr, "\n*** No <basefilename> given\n\n");
      SayUsage(); exit(4); }
   if (argc > argno + 1) {
      fprintf(stderr, "\n*** unknown parameter: %s\n\n", argv[argno]);
      SayUsage(); exit(4); }

   // break the commandline parameter into filename and extension, if one we recognize was given
   strlcpy(cmdfilename, argv[argno], sizeof(cmdfilename));
   char *filename_end = strrchr(cmdfilename, '.'); // last .
   if (filename_end &&
         (strcasecmp(filename_end, ".tap") == 0
          || strcasecmp(filename_end, ".csv") == 0
          || strcasecmp(filename_end, ".tbin") == 0)) {
      strlcpy(cmdfileext, filename_end, sizeof(cmdfileext)); // copy the extension, with the dot
      dlog("extension: %s\n", cmdfileext);
      *filename_end = 0; } // and remove it from the cmdfilename
   else cmdfileext[0] = 0;  // no extension was given

   //TODO: Move what follows into process_file so we do it for each file of a list?
   // Nah. A more elegant solution would be to gather all the options into a
   // structure, then save/restore it around the processing for each file, so each
   // file inherits what was given on the command line but can add/overrride.
   // That's probably overkill for a program that no one besides me is likely to use.
   if (!baseoutfilename_given) {
      assert(strlen(outpathname) + strlen(cmdfilename) < MAXPATH - 1, "path + basename too long");
      strcpy(baseoutfilename, outpathname);
      strcat(baseoutfilename, cmdfilename); }

   if (tap_read || strcasecmp(cmdfileext, ".tap") == 0) {  // we are only to read and interpret a SIMH .tap file
      ntrks = ntrks_specified; // -ntrks controls whether octal is 2 or 3 characters wide
      if (ntrks <= 0) ntrks = 9; // assume 9 tracks (3-digit octal) if not given
      if (txtfile_linesize == 0) txtfile_linesize = 64;
      show_program_info(argc, argv);
      read_tapfile(cmdfilename, cmdfileext);
      txtfile_close(); }

   else {  // do a real mag tape decoding
      assert(mode != WW || !multiple_tries, "Sorry, multiple decoding tries is not implemented yet for Whirlwind");
      start_time = time(NULL);
      if (filelist || strcasecmp(cmdfileext, ".txt") == 0) {  // process a list of files
         char filename[MAXPATH];
         strncpy(filename, cmdfilename, MAXPATH - 5); filename[MAXPATH - 5] = '\0';
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
               bool result = process_file(argc, argv, "");
               printf("%s: %s\n", baseinfilename, result ? "ok" : "bad"); } } }

      else {  // process one file
         strncpy(baseinfilename, cmdfilename, MAXPATH - 5);
         baseinfilename[MAXPATH - 5] = '\0';
         bool result = process_file(argc, argv, cmdfileext);
         double elapsed_time = difftime(time(NULL), start_time); // integral seconds, even though a double!
         bool skew_ok;
         // should move the following reports into process_file so we do it for a file list too
         if (quiet) {
            printf("%s: %s\n", baseinfilename, result ? "ok" : "bad"); }
         else {
            rlog("\n");
            open_summary_file(); {
               rlog("summary for file \"%s\":\n", indatafilename);
               rlog("  %s samples were processed in %.0lf seconds (%.3lf seconds/block)\n",
                    longlongcommas(lines_in), elapsed_time, numblks == 0 ? 0 : elapsed_time / numblks);
               rlog("  created %d output file%s with a total of %s bytes\n",
                    numfiles, numfiles != 1 ? "s" : "", longlongcommas(numoutbytes));
               rlog("  decoded %d tape marks and %d blocks with %s bytes from %.2lf seconds of tape data\n",
                    numtapemarks, numblks, longlongcommas(numdatabytes), timenow - data_start_time);
               if (last_block_time) rlog("  the last block written was %.8lf seconds into the tape\n", last_block_time);
               rlog("  %d block%s had errors, %d had warnings", numblks_err, numblks_err != 1 ? "s" : "", numblks_warn);
               if (mode != WW) rlog(", %d had mismatched tracks, %d had bits corrected", numblks_trksmismatched, numblks_corrected);
               if (mode == NRZI) rlog(", %d had midbit timing errors", numblks_midbiterrs);
               rlog("\n");
               if (mode == WW && num_flux_polarity_changes > 0) rlog("  the flux polarity changed %d time%s during decoding\n",
                        num_flux_polarity_changes, num_flux_polarity_changes > 1 ? "s" : "");
               if (numblks_unusable > 0) rlog("  %d blocks were unusable and were not written\n", numblks_unusable); }
            close_summary_file();
            if (multiple_tries) {
               rlog("  %d good blocks had to try more than one parmset\n", numblks_goodmultiple);
               for (int i = 0; i < MAXPARMSETS; ++i) //  // show stats on all the parameter sets we tried
                  if (parmsetsptr[i].tried > 0) {
                     rlog("  parmset %d was tried %4d times and used %4d times, or %5.1f%%\n",
                          i, parmsetsptr[i].tried, parmsetsptr[i].chosen, 100.*parmsetsptr[i].chosen / parmsetsptr[i].tried); } }
#if PEAK_STATS
            rlog("\n");
            output_peakstats("");
            skew_ok = skew_compute_deskew(false);
            open_summary_file(); {
               if (skew_ok) {
                  if (deskew) rlog("  deskewing with delays up to %.1f%% of a bit time seems to have been successful\n", deskew_max_delay_percent);
                  else rlog("  the tape data head skew is minimal\n"); }
               else {
                  if (deskew) rlog("  deskewing with delays up to %.1f%% of a bit time wasn't entirely effective\n"
                                      "  the tape might have been written by two different drives\n"
                                      "  if so you should consider separating the data into those sections\n", deskew_max_delay_percent);
                  else rlog("  head skew is significant; you should try again with the -deskew option\n"); } }
            close_summary_file();
#endif
         } // !quiet
         if (summcsvfilename[0]) {
            assert((summf = fopen(summcsvfilename, "a")) != NULLP, "can't open summary file %s", summcsvfilename);
            // the format here is odd, but it matches the spreadsheet we use to keep track of Whirlwind tape decodings
            fprintf(summf, "=\"%s\",=\"%s\",=\"%s\",=\"%s\", %.2lf, %d, %d, %lld, %d, %d, %d,\"%c\"\n",
                    baseinfilename,
                    tbin_hdr.u.s.flags & TBIN_INVERTED ? "yes" : "",
                    num_flux_polarity_changes == 0 ? (flux_direction_current == FLUX_POS ? "pos" : "neg") : "pos&neg",
                    track_order_string, timenow - data_start_time, numtapemarks, numblks, numdatabytes,
                    numblks_err, numblks_warn, num_flux_polarity_changes, skew_ok ? 'y' : 'n');
            fclose(summf); } } }

   return 0; }
//*
