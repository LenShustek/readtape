#!/usr/bin/make -f

READTAPE ?= readtape

all:
test:
	$(READTAPE) -v -m -gcr -ips=50 -order=76543210p -zeros -correct -tap -ascii -linefeed -outp=results/ 1kblks_43blks.tbin
	$(READTAPE) -v -m -gcr -ips=50                  -zeros -correct -tap -ascii -linefeed -outp=results/ sf93_8blks.tbin
	$(READTAPE) -v    -gcr -ips=125 -differentiate  -zeros          -tap -ascii           -outp=results/ analog.tbin
	for excepcted in expected_results/*.{tap,bin}; do \
		[ -r "$${expected}" ] || continue; \
		created="results/$$(basename "$${expected}")"; \
		cmp "$${expected}" "$${created}" || { echo "$${expected} does not match $${created}." >&2; exit 1; }; \
	done

.PHONY: all test
