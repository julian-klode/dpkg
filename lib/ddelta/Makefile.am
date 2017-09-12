## Process this file with automake to produce Makefile.in

noinst_LTLIBRARIES = libddelta.la

libddelta_la_CPPFLAGS = $(DIVSUFSORT_CFLAGS) -DDDELTA_NO_MAIN=1
libddelta_la_LIBADD = $(DIVSUFSORT_LIBS)
libddelta_la_SOURCES = \
    ddelta_generate.c \
    ddelta_apply.c

noinst_PROGRAMS = ddelta_apply ddelta_generate

ddelta_generate_LDADD = $(DIVSUFSORT_LIBS)
ddelta_generate_CPPFLAGS = $(DIVSUFSORT_CFLAGS)
ddelta_generate_SOURCES = ddelta_generate.c
ddelta_apply_SOURCES = ddelta_apply.c