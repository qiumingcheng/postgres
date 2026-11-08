#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for statistics
#
# IDENTIFICATION
#    src/backend/statistics/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/statistics
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	attribute_stats.o \
	dependencies.o \
	extended_stats.o \
	extended_stats_funcs.o \
	graph_stats_collect.o \
	graph_stats_feedback.o \
	graph_stats_path_collect.o \
	graph_stats_store.o \
	mcv.o \
	mvdistinct.o \
	relation_stats.o \
	stat_utils.o

include $(top_srcdir)/src/backend/common.mk
