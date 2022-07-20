
This is a program for recovering data from old magnetic computer tapes by 
digitizing the analog waveforms and then using software to decode the bits 
and reconstruct the original data. The objective is to correctly read tapes 
that have degraded beyond the point where conventional tape drives will 
work, or for which drives are no longer available. We have had good success 
using this for tapes in the collection of the Computer History Museum. 

This lives at https://github.com/LenShustek/readtape. For a slide show
about the system, see VCF_Aug2020_01.pdf. For a low-quality video of me
giving a talk about it, see https://www.youtube.com/watch?v=7YoolSAHR5w&t=4200s.

For lots of detailed information about how to use it, and how it works, 
see A_documentation.txt. For a detailed change log, see the beginning
of src\readtape.c.

We record data using a 16-channel Saleae digital/analog logic analyzer 
(https://www.saleae.com) connected to any of several computer-grade magnetic 
tape decks, including the Qualstar 1052 with either a 9-track or 7-track 
head installed, the Qualstar 3418S, and the Kennedy 9600. The input is 
typically taken from the output of a differential amplifier that produces 
a +-2V or larger analog signal for reasonable tapes. We ignore the drive's 
subsequent circuitry (thresholding, zero-crossing identification, envelope 
detection, etc.) that tries to recreate the data bits on the tape. 

I had first designed and prototyped a custom 9-channel A-to-D converter
using the amazing Cirrus/Wolfson WM8235 9-channel analog front end, 
http://www.mouser.com/ds/2/76/WM8235_v4.2-532446.pdf. But I was unable
to find the right setting for their 300 configuration registers that made
it work as a normal A-to-D converter, and their tech support would not
respond to inquiries. The remnants of that suspended project are in the
front_end directory.

The data exported by the logic analyzer is a comma-separated-value (CSV) 
file whose lines contains a timestamp and the voltages for all the read 
heads. We set the sampling rate to generate about 20 samples per cycle. 
For 800 BPI NRZI tapes read at 50 IPS, the Saleae 781 KHz rate works well. 

But the CSV files can be huge -- many tens of gigabytes for a few 
minutes of recording -- so for archival purposes we've defined a binary 
compressed "TBIN" format, and I wrote the utility program "csvtbin" that 
can convert between CSV and TBIN. The "readtape" decoding program can read
either format. The compression using TBIN is about 10:1, and it speeds up 
decoding by about 2x. 

The output of the decoding can include:
 - a log file
 - multiple binary files of the reconstructed data separated at filemarks, or
 - one SIMH .tap file that encodes data and filemarks
     (see http://simh.trailing-edge.com/docs/simh_magtape.pdf)
 - a text file in various formats of readable numeric and character interpretation
 - a CSV file with data showing peak dispersion after track deskewing
 - a CSV file, in DEBUG mode, that recreates one or all tracks of data
    with information about the state of the decoding, like peaks detected

We so far support 7-track NRZI format, 9-track NRZI, PE, GCR formats, and,
most recently, the bizarre 6-track tapes that were written on the vacuum-
tube Whirlwind I computer. The museum has over a hundred of those, and we
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
 VCF_Aug2020_01.pdf     The slide show about the project
 src\readtape.c         The main source file, which also has the complete change
                        log and notes about the internal program structure
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
 src\tapread.c           a .tap file reader in support of the -tapread option
 
---UTILITY PROGRAMS

 src\csvtbin.c           a program for converting between CSV and TBIN files
 src\dumptap.c           a deprecated program for dumping SIMH .tap files
                         (but this functionality, expanded, is now an option in readtape)
---BINARIES
 bin\readtape.exe        readtape Windows 64-bit (x64) executable
 bin\csvtbin.exe         csvtbin Windows 64-bit (x64) executable
 
---TEST DATA
 examples\README.txt     a directory with test magnetic tape data and decodes
                         and batch files to run them automatically
 
*** Thanks to: 
 - Paul Pierce for the original inspiration of his similar work 10+ years ago.
 - Grant Saviers for detailed consulting on tape nitty-gritties. 
 - Al Kossow for the tape drive, for making lots of good suggestions (Saleae, 
   .tap format, compressed files, etc.), and for carefully reading many, many 
   tapes from the collection of the Computer History Museum.

Len Shustek
 6 Feb 2018
17 May 2018, 27 May 2018, 8 Oct 2018
 4 Aug 2019, 29 Dec 2019, 28 Feb 2022, 21 Jun 2022, 20 Jul 2022

