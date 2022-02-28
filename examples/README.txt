This directory contains sample data files in .tbin format so you can test your build of the software.

There are
 2 files each for 7-track NRZI, 9-track NRZI, and 9-track PE,
 3 files for 9-track GCR, one of which requires -differentiation,
 1 file for 6-track Whirlwind,
so a total of 10 tests. The correct results are in the various "expected_results" directories.

Tape blocks were selected so that the .tbin files are (mostly) less than 50 MB, beyond which GitHub complains.

You can run all the tests with "runtests.bat", which puts your output files in the various "results" directories.
The batch file then compares the .tap or.bin files in the "results" and "expected_result" directories, and
pauses if there are differences. The various text files (.log, .txt) might have minor inconsequential differences.
