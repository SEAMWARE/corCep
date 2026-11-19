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
#   make coverage        the functional tests on an instrumented corCep (obj/coverage/corCep): coverage/index.html
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
else ifeq ($(BUILD),coverage)
  OPT       = -g -O0 --coverage -fprofile-update=atomic   # atomic: no negative counts from the threads (GCC bug 68080)
else
  OPT       = -O2
endif

#
# DRY_RUN - 1 | 0. POST /dry-run (src/dryrun.c): rules replayed against made-up events, for the rule editors -
# and corNgsild's q matcher, corJsonld and corHash with it. 0 leaves all of it out.
#
DRY_RUN    ?= 1

ifeq ($(DRY_RUN),1)
  DRY_RUN_SOURCES  = src/dryrun.c
  DRY_RUN_ARCHIVES = $(COR_LIBS)/corNgsild/libcorNgsild.a $(COR_LIBS)/corJsonld/libcorJsonld.a
  DRY_RUN_HASH     = $(COR_LIBS)/corHash/libcorHash.a
else ifeq ($(DRY_RUN),0)
  DRY_RUN_SOURCES  =
  DRY_RUN_ARCHIVES =
  DRY_RUN_HASH     =
else
  $(error DRY_RUN must be 1 or 0, not '$(DRY_RUN)')
endif

CFLAGS      = $(OPT) -I$(OBJDIR) -Wall -Werror -Wundef -Wno-format-truncation -fstack-protector-all -DCEP_WITH_DRY_RUN=$(DRY_RUN) -I$(COR_LIBS) -MMD -MP $(EXTRA_CFLAGS)

#
# The engine - libcorcep.a: rules, patterns, schedules, conditions, actions, statistics, state, ${...}, the author's
# rights. It reaches the broker through a CepTransport (src/cepTransport.h).
# The standalone corCep - the engine on NGSI-LD over HTTP: main, the requests, the subscriptions and their
# listener, the actions' queues, the token, /dry-run.
#
ENGINE_SOURCES = src/engine.c src/rules.c src/pattern.c src/schedule.c src/state.c src/vars.c src/keys.c src/author.c src/util.c
CEP_SOURCES    = src/corCep.c src/transportHttp.c src/broker.c src/subs.c src/listener.c src/token.c src/sender.c $(DRY_RUN_SOURCES)
SOURCES        = $(ENGINE_SOURCES) $(CEP_SOURCES)
OBJDIR      = obj/$(BUILD)
OBJECTS     = $(SOURCES:src/%.c=$(OBJDIR)/%.o)
ENGINE_OBJECTS = $(ENGINE_SOURCES:src/%.c=$(OBJDIR)/%.o)
CEP_OBJECTS    = $(CEP_SOURCES:src/%.c=$(OBJDIR)/%.o)
DEPS        = $(OBJECTS:.o=.d) $(patsubst src/%.c,obj/$(BUILD)-pic/%.d,$(ENGINE_SOURCES) src/cepPlugin.c src/transportHttp.c src/broker.c src/subs.c src/token.c src/sender.c)

#
# Link order: a library before the ones it uses
#
COR_ARCHIVES = $(DRY_RUN_ARCHIVES)               \
               $(COR_LIBS)/corRest/libcorRest.a   \
               $(COR_LIBS)/corHttp/libcorHttp.a   \
               $(DRY_RUN_HASH)                    \
               $(COR_LIBS)/corJson/libcorJson.a   \
               $(COR_LIBS)/corTree/libcorTree.a   \
               $(COR_LIBS)/corArgs/libcorArgs.a   \
               $(COR_LIBS)/corLog/libcorLog.a     \
               $(COR_LIBS)/corAlloc/libcorAlloc.a \
               $(COR_LIBS)/corBase/libcorBase.a

LIBS        = -Wl,--start-group $(COR_ARCHIVES) -Wl,--end-group -lssl -lcrypto -lz -lpthread -lm

all: corCep

corCep: $(CEP_OBJECTS) $(OBJDIR)/libcorcep.a $(COR_ARCHIVES)
	$(CC) $(EXTRA_LDFLAGS) -o $@ $(CEP_OBJECTS) $(OBJDIR)/libcorcep.a $(LIBS)

$(OBJDIR)/libcorcep.a: $(ENGINE_OBJECTS)
	rm -f $@
	ar rcs $@ $(ENGINE_OBJECTS)

libcorcep.a: $(OBJDIR)/libcorcep.a
	cp $< $@

#
# cep.so - corCep inside coraine (coraine --bridges cep): the engine, the HTTP requests and the actions' queues,
# position-independent, the cor-libs NOT linked - they are coraine's (exported). No listener, no /dry-run.
# Installed as <coraine prefix>/plugins/bridge/cep.so; built in obj/<build>-pic.
#
PIC_DIR     = obj/$(BUILD)-pic
PLUGIN_SOURCES = $(ENGINE_SOURCES) src/cepPlugin.c src/transportHttp.c src/broker.c src/subs.c src/token.c src/sender.c
PLUGIN_OBJECTS = $(PLUGIN_SOURCES:src/%.c=$(PIC_DIR)/%.o)

cep.so: $(PLUGIN_OBJECTS)
	$(CC) $(EXTRA_LDFLAGS) -shared -o $@ $(PLUGIN_OBJECTS) -lpthread -lm

$(PIC_DIR)/%.o: src/%.c
	@mkdir -p $(PIC_DIR)
	$(CC) $(subst -DCEP_WITH_DRY_RUN=1,-DCEP_WITH_DRY_RUN=0,$(CFLAGS)) -fPIC -fvisibility=hidden -c $< -o $@

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

#
# stack.h - what --version prints: corCep's commit and each cor-lib's, as built (git describe of each checkout;
# "unknown" where one is no git checkout - a source tarball). Written on every build, replaced only when it changes,
# so it rebuilds corCep.o only then.
#
STACK_REPOS = corCep corBase corLog corAlloc corTree corJson corHash corArgs corHttp corRest corJsonld corNgsild

$(OBJDIR)/corCep.o: $(OBJDIR)/stack.h

$(OBJDIR)/stack.h: FORCE
	@mkdir -p $(OBJDIR)
	@{ printf '#define CORCEP_STACK \\\n'; \
	   for r in $(STACK_REPOS); do \
	     d=$(COR_LIBS)/$$r; [ $$r = corCep ] && d=.; \
	     v=$$(git -C $$d describe --always --dirty --abbrev=12 2>/dev/null || echo unknown); \
	     printf '  "  %-10s %s\\n" \\\n' $$r "$$v"; \
	   done; printf '  ""\n'; } > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

FORCE:

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

#
# coverage - how much of corCep the functional tests run: corCep built with --coverage into obj/coverage (from
# scratch: a stale .gcno of a source gone counts as never run), the suite run on THAT binary (CEP_BIN - ./corCep is
# left as it is), then gcovr: a summary on the terminal, line by line in coverage/index.html.
#
# corCep's own src/ only. The cor-libs it links are not instrumented: they are sibling checkouts coraine builds
# from too, and instrumenting them there would leave them -O0 --coverage for whatever is built next.
# A corCep stopped with kill -9 (a test's crash) writes no counts: its lines count as run only if another one ran
# them. The broker must be up: test/funcTests/broker.sh up (as for make functest).
#
# gcovr >= 8.3 (--gcov-ignore-parse-errors): GCOVR=<path> if it is not on PATH.
#
GCOVR        ?= $(shell command -v gcovr || echo $(HOME)/git/ngsi-ld-test-suite/.venv/bin/gcovr)

coverage:
	rm -rf obj/coverage coverage
	$(MAKE) BUILD=coverage obj/coverage/corCep
	CEP_BIN=$(CURDIR)/obj/coverage/corCep $(CORTEST) $(CORTEST_ARGS); echo $$? > obj/coverage/suite-status
	mkdir -p coverage
	$(GCOVR) --root . -f 'src/' --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
	         --html-details coverage/index.html --html-title "corCep coverage" \
	         --txt coverage/summary.txt --print-summary obj/coverage
	@cat coverage/summary.txt
	@echo ""
	@echo "Coverage report: file://$(CURDIR)/coverage/index.html"
	@status=$$(cat obj/coverage/suite-status); if [ "$$status" != 0 ]; then \
	   echo "*** the suite FAILED (corTest exited $$status) - the report measures a suite that is not passing"; exit $$status; fi

#
# valgrind - the functional tests with every long-running corCep under valgrind (corTest --vt): a test fails on a
# definite or indirect leak or a memory error. corCep built -g -O0 into obj/debug (./corCep is left as it is); the
# broker container started and stopped as for make functest. Before every release.
#
valgrind:
	$(MAKE) BUILD=debug obj/debug/corCep
	test/funcTests/broker.sh up
	CEP_BIN=$(CURDIR)/obj/debug/corCep $(CORTEST) --vt $(CORTEST_ARGS); rc=$$?; test/funcTests/broker.sh down; exit $$rc

$(OBJDIR)/corCep: $(CEP_OBJECTS) $(OBJDIR)/libcorcep.a $(COR_ARCHIVES)
	$(CC) $(EXTRA_LDFLAGS) $(if $(filter coverage,$(BUILD)),--coverage) -o $@ $(CEP_OBJECTS) $(OBJDIR)/libcorcep.a $(LIBS)

install: corCep
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 corCep $(DESTDIR)$(PREFIX)/bin/corCep

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/corCep

clean:
	rm -rf obj corCep libcorcep.a cep.so coverage

.PHONY: all libcorcep.a test functest coverage valgrind install uninstall clean FORCE

-include $(DEPS)
