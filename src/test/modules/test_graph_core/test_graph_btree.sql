CREATE FUNCTION test_graph_btree_provider(regclass, regclass, integer, bigint[],
                                        integer, integer DEFAULT 0,
                                        integer DEFAULT 0, integer DEFAULT 0,
                                        integer DEFAULT 0, oid DEFAULT 0)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_btree_provider'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_btree_mark_restore(regclass, regclass, boolean,
                                             boolean, boolean DEFAULT false)
RETURNS void
AS 'MODULE_PATHNAME', 'test_graph_btree_mark_restore'
LANGUAGE C STRICT PARALLEL UNSAFE;
