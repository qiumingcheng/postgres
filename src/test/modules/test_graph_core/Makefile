# Common diagnostics build without either concrete graph provider.
graph_test_srcdir := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
MODULE_big = test_graph_core
OBJS = test_graph_core.o test_graph_provenance.o test_graph_batch.o test_graph_batch_mapping.o test_graph_fixed.o test_graph_cost.o test_graph_plan.o
EXTENSION = test_graph_core
DATA_built = test_graph_core--1.0.sql
REGRESS = graph_stats_collect graph_stats_reassign graph_provenance graph_plan
graph_sql_parts = $(graph_test_srcdir)test_graph_core.sql
-include $(graph_test_srcdir)btree.mk
-include $(graph_test_srcdir)adj.mk

# These tests require both providers; neither standalone package runs them.
ifneq ($(wildcard $(graph_test_srcdir)btree.mk),)
ifneq ($(wildcard $(graph_test_srcdir)adj.mk),)
REGRESS += graph_lifecycle graph_route
endif
endif

REGRESS_OPTS = --load-extension=test_graph_core
PGFILEDESC = "test_graph_core - graph node and provider ABI tests"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_graph_core
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

.PHONY: graph_sql_force
test_graph_core--1.0.sql: $(graph_sql_parts) graph_sql_force
	cat $(graph_sql_parts) > $@
