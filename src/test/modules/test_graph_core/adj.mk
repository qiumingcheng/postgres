OBJS += test_graph_adj.o
PG_CPPFLAGS += -DGGC_TEST_HAVE_GRAPH_ADJ
REGRESS += graph_adj_provider graph_adj_fixed
graph_sql_parts += $(graph_test_srcdir)test_graph_adj.sql
