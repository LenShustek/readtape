#!/usr/bin/make -f

READTAPE ?= readtape

all:
test:
	$(READTAPE) -whirlwind -v3 -fluxdir=auto -tap -deskew -octal2 -flexo -outp=results/ 132_pt1.tbin
	for excepcted in expected_results/*.{tap,bin}; do \
		[ -r "$${expected}" ] || continue; \
		created="results/$$(basename "$${expected}")"; \
		cmp "$${expected}" "$${created}" || { echo "$${expected} does not match $${created}." >&2; exit 1; }; \
	done

.PHONY: all test
