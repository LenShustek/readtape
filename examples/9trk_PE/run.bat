readtape -v -m -ntrks=9 -pe -bpi=1600 -ips=50 -order=01234576p -tap -ascii -linefeed -outp=results\ %1 %2 %3  1600bpi_ukn_6s
readtape -v -m -ntrks=9 -pe -bpi=1600 -ips=50 -tap -ebcdic -linesize=137 -outp=results\ %1 %2 %3 LJS009_part1_39blks
comp /M expected_results\*.tap results
@echo off
if %ERRORLEVEL% NEQ 0 pause .tap files differ