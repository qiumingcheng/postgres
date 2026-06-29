# src/backend/optimizer/cascades/Makefile

subdir = src/backend/optimizer/cascades
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = cascades.o debug.o postopt.o \
       core/memo.o core/task.o core/property.o core/pattern.o \
       core/planbuild.o core/rewrite.o core/registry.o \
       adapters/pg_adapter.o adapters/pg_plan.o adapters/pg_stats.o \
       modules/scan.o modules/filter.o modules/join.o \
       modules/agg.o modules/sort.o modules/limit.o \
       modules/distinct.o modules/project.o modules/union.o

# Add cascades/ to include path so .c files can #include "core/registry.h"
override CPPFLAGS += -I$(srcdir)

include $(top_srcdir)/src/backend/common.mk
