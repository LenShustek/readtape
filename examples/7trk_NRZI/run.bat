readtape -v -m -nrzi -ntrks=7 -order=543210p -tap -SDS -linesize=144 -outp=results\ %1 %2 %3 SRI_SDS_102715028_4secs
readtape -v -m -nrzi -ntrks=7 -tap -outp=results\ %1 %2 %3 tss_4secs
comp /M expected_results\*.tap results
@echo off
if %ERRORLEVEL% NEQ 0 pause .tap files differ