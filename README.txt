      UPDATE: Here is a link to the video of my talk about this
      project at the August 2020 Vintage Computer Festival West: 
      https://youtu.be/7YoolSAHR5w?t=4200. 

This is a program for recovering data from old magnetic computer tapes
by digitizing the analog waveforms and then using software to decode the 
bits and reconstruct the original data. The objective is to correctly 
read tapes that have degraded beyond the point where conventional tape 
drives will work. We have had good success using this for tapes in the
collection of the Computer History Museum.

We are currently recording data using a 16-channel Saleae logic analyzer 
(https://www.saleae.com) connected to a Qualstar 1052 tape transport 
(http://bitsavers.org/pdf/qualstar/500150B_1052serviceMan.pdf). The 
input is taken from the output of a differential amplifier that produces 
a +-2V or larger analog signal for reasonable tapes. We ignore the 
drive's subsequent thresholding circuitry that tries to recreate the 
data bits on the tape.

We have also used other tape drives, in particular for 9-track GCR tapes
recorded at 6250 BPI.

(I had first designed and prototyped a custom 9-channel A-to-D converter
using the amazing Cirrus/Wolfson WM8235 9-channel analog front end, 
http://www.mouser.com/ds/2/76/WM8235_v4.2-532446.pdf. But I was unable
to find the right setting for their 300 configuration registers that made
it work as a normal A-to-D converter, and their tech support would not
respond to inquiries. It is on hold, but I may someday get back to it.)

The data exported by the logic analyzer is a comma-separated-value (CSV) 
file whose lines contains a timestamp and the voltages for all the read 
heads. We set the sampling rate to generate about 20 samples per cycle. 
For 800 BPI NRZI tapes read at 50 IPS, the Saleae 781 KHz rate works well. 

But the CSV files can be huge -- many tens of gigabytes for a few 
minutes of recording -- so for archival purposes we've defined a binary 
compressed "TBIN" format, and I wrote a utility program that can convert 
between CSV and TBIN. The "readtape" decoding program can read either 
format. The compression using TBIN is about 10:1, and it speeds up 
decoding by about 2x. 

We so far support 7-track NRZI format, 9-track NRZI, PE, GCR formats, and,
most recently, the bizarre 6-track tapes that were written on the vacuum-
tube Whirlwind I computer. The museum has over a hundred of them, and we
have had remarkable success (about 95%) in recovering data and programs 
that have been unread and unexamined for fifty years. 

*** The files in this repository

 (Github ought to allow the file list of a repository to say
  what the files *are*, not what the last minor edit was!
  https://github.community/t5/How-to-use-Git-and-GitHub/Naive-question-about-describing-files/td-p/7532)

---DOCUMENTATION

 A_documentation.txt    A narrative about usage and internal operation
 A_experiences.txt      Some (old) anecdotes about what we have done with this
 AtoD_attachment.jpg    A photo showing how the analyzer connects to the drive
 example_01.pdf         An example of a really bad block we can decode
 flux_transition_dispersion.jpg  A graph showing the effect of head skew
 analog_front_end.pdf   The schematic of my original design for the A-to-D converter
 
---READTAPE source code

 src\readtape.c          main program: options, file handling, and block processing
 src\decoder.h           compile-time options, and common declarations
 src\csvtbin.h           the format of the .tbin compressed binary data file
 src\decoder.c           common routines for analog sample analysis and decoding
 src\decode_pe.c         PE (phase encoded) decoding routines
 src\decode_nrzi.c       NRZI (non-return-to-zero-inverted) decoding routines 
 src\decode_gcr.c        GCR (group coded recording) decoding routines
 src\decode_ww.c         Whirlwind I 6-track decoding routines
 src\parmsets.c          parameter set processing, and their defaults
 src\textfile.c          interpreted text dump of the data
 src\ibmlabels.c         IBM 9-track standard label (SL) interpretation
 src\trace.c             create debugging output and spreadsheet graphs
 
---UTILITY PROGRAMS

 src\csvtbin.c           a program for converting between CSV and TBIN files
 src\dumptap.c           a program for dumping SIMH .tap files, one of the output formats
                         (but this functionality, expanded, is now an option in readtape)
---BINARIES
 bin\readtape.exe        Windows 64-bit executable
 bin\csvtbin.exe         Windows 64-bit executable
 
---TEST DATA
 examples\README.txt     a directory with test magnetic tape data and decodes
 
*** Thanks to: 
 - Paul Pierce for the original inspiration of his similar work 10+ years ago.
 - Grant Saviers for detailed consulting on tape nitty-gritties. 
 - Al Kossow for the tape drive, for making lots of good suggestions (Saleae, 
   .tap format, compressed files, etc.), and for carefully reading many, many 
   tapes from the collection of the Computer History Museum.

Len Shustek
 6 Feb 2018
17 May 2018, 27 May 2018, 8 Oct 2018
 4 Aug 2019, 29 Dec 2019

