readtape -v -m -gcr -ips=50 -order=76543210p -zeros -correct -tap -ascii -linefeed -outp=results\ %1 %2 %3 1kblks_43blks
readtape -v -m -gcr -ips=50 -zeros -correct -tap -ascii -linefeed %1 %2 %3 -outp=results\ sf93_8blks
readtape -v -gcr -ips=125 -differentiate -zeros -tap -ascii %1 %2 %3 -outp=results\ analog
comp /M expected_results\*.tap results
@echo off
if %ERRORLEVEL% NEQ 0 pause .tap files differ