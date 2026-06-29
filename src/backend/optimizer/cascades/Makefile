# src/backend/optimizer/cascades/Makefile

subdir = src/backend/optimizer/cascades
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = cascades.o memo.o task.o rule.o property.o pattern.o \
       pg_adapter.o planbuild.o debug.o postopt.o rewrite.o \
       core/registry.o \
       modules/scan.o modules/filter.o modules/join.o \
       modules/agg.o modules/sort.o modules/limit.o \
       modules/distinct.o modules/project.o

include $(top_srcdir)/src/backend/common.mk
