This directory contains sample data files in .tbin format so you can test your build of the software.
There are two files each for 7-track NRZI, and for 9-track NRZI, PE, and GCR, 
and one for 6-track Whirlwind, for a total of 9 tests. 
Tape blocks were selected so that the .tbin files are (mostly) less than 50 MB, beyond which GitHub complains.
The correct results are in the various "expected_results" directories.
You can run all the tests with "runtests.bat", which puts your output files in the various "results" directories.
Comparing the files in the "results" and "expected_results" directories should show only minor inconsequential differences.
