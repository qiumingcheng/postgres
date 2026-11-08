OBJS += test_graph_btree.o
REGRESS += graph_btree graph_stats_path graph_stats_cost graph_batch graph_fixed graph_cbo graph_pgq graph_composition graph_local_quals graph_properties graph_directions graph_regions graph_orders graph_mappings graph_integer_acl
REGRESS += graph_native
REGRESS += graph_auto_stats
REGRESS += graph_native_reuse graph_index_cost graph_binding_reuse
graph_sql_parts += $(graph_test_srcdir)test_graph_btree.sql
REGRESS += graph_stage_cost graph_batch_resources graph_limit_stream
