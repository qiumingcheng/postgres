CREATE FUNCTION test_graph_adj_walk(regclass, regclass, bigint[], integer,
                                    integer DEFAULT 1, integer DEFAULT 0)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_adj_walk'
LANGUAGE C STRICT PARALLEL UNSAFE;
