#!/usr/bin/make -f

READTAPE ?= readtape

all:
test:
	$(READTAPE) -v -m -nrzi -ntrks=7 -order=543210p -tap -SDS -linesize=144 -outp=results/ SRI_SDS_102715028_4secs.tbin
	$(READTAPE) -v -m -nrzi -ntrks=7                -tap                    -outp=results/ tss_4secs.tbin
	for excepcted in expected_results/*.{tap,bin}; do \
		[ -r "$${expected}" ] || continue; \
		created="results/$$(basename "$${expected}")"; \
		cmp "$${expected}" "$${created}" || { echo "$${expected} does not match $${created}." >&2; exit 1; }; \
	done

.PHONY: all test
