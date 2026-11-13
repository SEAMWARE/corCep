#
# FILE            makefile
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corCep - an NGSI-LD complex event processor
#
#   make                 the corCep binary (in this directory)
#   make functest        build, and run the functional tests (corTest) against a coraine container it starts
#   make test            the same as make functest
#   make install         $(PREFIX)/bin/corCep (PREFIX=/usr/local; DESTDIR for packaging)
#   make uninstall
#   make clean
#   make BUILD=debug EXTRA_CFLAGS=-fsanitize=address EXTRA_LDFLAGS=-fsanitize=address   (AddressSanitizer)
#
# Built on the cor-libs, as coraine is: SIBLING checkouts (../corRest, ../corJson, ...) built there,
# `-I..` and the archives linked by path. COR_LIBS=<dir> for checkouts elsewhere.
#
CC          = gcc
COR_LIBS   ?= ..
PREFIX     ?= /usr/local
DESTDIR    ?=
BUILD      ?= release

ifeq ($(BUILD),debug)
  OPT       = -g -O0
else
  OPT       = -O2
endif

CFLAGS      = $(OPT) -Wall -Werror -Wundef -Wno-format-truncation -fstack-protector-all -I$(COR_LIBS) -MMD -MP $(EXTRA_CFLAGS)

SOURCES     = src/corCep.c src/broker.c src/rules.c src/subs.c src/listener.c src/engine.c src/pattern.c
OBJDIR      = obj/$(BUILD)
OBJECTS     = $(SOURCES:src/%.c=$(OBJDIR)/%.o)
DEPS        = $(OBJECTS:.o=.d)

#
# Link order: a library before the ones it uses
#
COR_ARCHIVES = $(COR_LIBS)/corRest/libcorRest.a   \
               $(COR_LIBS)/corHttp/libcorHttp.a   \
               $(COR_LIBS)/corJson/libcorJson.a   \
               $(COR_LIBS)/corTree/libcorTree.a   \
               $(COR_LIBS)/corArgs/libcorArgs.a   \
               $(COR_LIBS)/corLog/libcorLog.a     \
               $(COR_LIBS)/corAlloc/libcorAlloc.a \
               $(COR_LIBS)/corBase/libcorBase.a

LIBS        = $(COR_ARCHIVES) -lssl -lcrypto -lz -lpthread -lm

all: corCep

corCep: $(OBJECTS) $(COR_ARCHIVES)
	$(CC) $(EXTRA_LDFLAGS) -o $@ $(OBJECTS) $(LIBS)

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COR_ARCHIVES):
	@echo "missing $@ - corCep builds against sibling checkouts of the cor-libs, built there (make in each), or COR_LIBS=<dir>"
	@false

#
# functest - test/funcTests/cases/*.test, run by corTest (the cor stack's test runner: corLibs/bin, or
# CORTEST=<path>), against ONE broker for the run - a coraine container on the host's network that
# test/funcTests/broker.sh starts and, whatever the result, stops. The tests isolate themselves by tenant.
# CORTEST_ARGS for corTest's own options (a single test, --tags, -v).
#
# Against a broker of your own instead: CEP_BROKER_HOST=... CEP_BROKER_PORT=... corTest
#
CORTEST      ?= corTest
CORTEST_ARGS ?=

functest: corCep
	test/funcTests/broker.sh up
	$(CORTEST) $(CORTEST_ARGS); rc=$$?; test/funcTests/broker.sh down; exit $$rc

test: functest

install: corCep
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 corCep $(DESTDIR)$(PREFIX)/bin/corCep

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/corCep

clean:
	rm -rf obj corCep

.PHONY: all test functest install uninstall clean

-include $(DEPS)
