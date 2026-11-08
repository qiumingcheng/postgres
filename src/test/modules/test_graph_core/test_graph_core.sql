\echo Use "CREATE EXTENSION test_graph_core" to load this file. \quit

CREATE FUNCTION test_graph_node_contract()
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_node_contract'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_provider_contract()
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_provider_contract'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_pgq_provenance(text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_pgq_provenance'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_pgq_statistics_scope(text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_pgq_statistics_scope'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_pgq_invalid_mapping(text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_pgq_invalid_mapping'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_batch(regclass, regclass, integer, bigint[], bigint[],
                                 integer DEFAULT 2, integer DEFAULT 8,
                                 integer DEFAULT 4, integer DEFAULT 65536,
                                 integer DEFAULT 1048576, boolean DEFAULT false,
                                 integer DEFAULT 0, integer DEFAULT 1,
                                 boolean DEFAULT true, integer DEFAULT 0,
                                 integer DEFAULT 0)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_batch'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_batch_cleanup_state()
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_batch_cleanup_state'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_fixed_config(regclass, regclass[], regclass[], integer[],
                                        integer DEFAULT 0, integer DEFAULT 4,
                                        integer DEFAULT 8, integer DEFAULT 100000)
RETURNS integer
AS 'MODULE_PATHNAME', 'test_graph_fixed_config'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_fixed_tuning(integer, integer, integer, bigint,
                                        boolean, boolean)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_graph_fixed_tuning'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_lifecycle_config(text, regclass[], bigint, bigint,
                                            bigint, oid)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_lifecycle_config'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_lifecycle_diagnostic()
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_lifecycle_diagnostic'
LANGUAGE C PARALLEL UNSAFE;

CREATE FUNCTION test_graph_cbo_config(regclass, regclass, regclass, oid, oid,
                                      integer, bigint, bigint, bigint, bigint,
                                      integer DEFAULT 2, integer DEFAULT 1)
RETURNS integer
AS 'MODULE_PATHNAME', 'test_graph_cbo_config'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_cbo_diagnostic()
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_cbo_diagnostic'
LANGUAGE C PARALLEL UNSAFE;

CREATE FUNCTION test_graph_path_cost(oid, oid, oid, integer, integer,
                                     bigint, bigint, bigint, bigint,
                                     integer, integer, double precision,
                                     double precision[], integer, bigint)
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_path_cost'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_feedback_buffer()
RETURNS text
AS 'MODULE_PATHNAME', 'test_graph_feedback_buffer'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_graph_feedback_security(oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_graph_feedback_security'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Exercise common endpoint grouping without depending on a concrete AM.
CREATE FUNCTION test_graph_batch_mapping(integer, integer, boolean)
RETURNS boolean AS 'MODULE_PATHNAME', 'test_graph_batch_mapping'
LANGUAGE C STRICT PARALLEL UNSAFE;
