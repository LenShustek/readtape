#!/usr/bin/make -f

SUBDIRS := $(wildcard */.)
READTAPE ?= ../../src/readtape

all:
test: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) READTAPE=$(READTAPE) -C $@ test

.PHONY: all test $(SUBDIRS)
