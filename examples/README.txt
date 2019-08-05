This directory contains sample data files in .tbin format so you can test your build of the software.
There are two files each for 7-track NRZI, and for 9-track NRZI, PE, and GCR, so a total of 8. 
Records were discarded so that the .tbin files are (mostly) less than 50 MB, beyond which GitHub complains.
The results you should get are in the various "\expected_results" directories.
You can run all the tests with "runtests.bat", which puts your output files in the various \results directories.
Comparing the files in the "results\" and "expected_results" directories should show only minor inconsequential differences.
