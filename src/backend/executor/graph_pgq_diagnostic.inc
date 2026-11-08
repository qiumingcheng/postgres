/* Explicit test-module registration and diagnostic configuration only.
 * Not a SQL-facing production API: GgcGraphDiagnosticCall requires an active
 * registered diagnostic provider. Included by graph_pgq.c to keep these
 * private helpers out of the formal planner/executor interfaces. */

void
GgcRegisterGraphDiagnostics(const GgcGraphDiagnosticOps *ops)
{
	static bool gucs_registered;

	if (!ops || ops->struct_size != sizeof(*ops) ||
		ops->version != GGC_GRAPH_DIAGNOSTIC_VERSION ||
		!ops->wrap || !ops->reset_counters || !ops->get_counters)
		elog(ERROR, "invalid graph diagnostic registration");
	if (diagnostic_ops && diagnostic_ops != ops)
		elog(ERROR, "graph diagnostics already registered");
	diagnostic_ops = ops;
	if (gucs_registered)
		return;
	DefineCustomBoolVariable("test_graph_core.btree_seek_hint",
		"Reuse freshly rechecked internal-page search positions in graph array scans.", NULL,
		&graph_btree_seek_hint, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.btree_batch_route",
		"Batch route graph array keys through a shared internal page.", NULL,
		&graph_btree_batch_route, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.btree_int8_compare",
		"Use proven single-bigint graph search comparisons.", NULL,
		&graph_btree_int8_compare, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.vertex_pointer_sort",
		"Sort bounded vertex batches by reference instead of moving handles.", NULL,
		&graph_vertex_pointer_sort, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.edge_range_probe",
		"Use a range for a proven contiguous batch of edge keys.", NULL,
		&graph_edge_range_probe, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.vertex_range_probe",
		"Use a range for a proven contiguous batch of vertex keys.", NULL,
		&graph_vertex_range_probe, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.endpoint_pipeline",
		"Consume eligible endpoint-set edges in bounded value chunks.", NULL,
		&graph_endpoint_pipeline, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("test_graph_core.endpoint_batch_size",
		"Declared endpoint request budget for standard PGQ endpoint sets.", NULL,
		&graph_endpoint_batch_size, 8, 1, 128, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("test_graph_core.vertex_batch_size",
		"Bounded vertex membership batch size for endpoint sets.", NULL,
		&graph_vertex_batch_size, 32, 1, 128, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.edge_index_only",
		"Read covered endpoint-set edges from all-visible index tuples.", NULL,
		&graph_edge_index_only, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.edge_probe_batch",
		"Use bounded multi-key edge probes in unordered endpoint sets.", NULL,
		&graph_edge_probe_batch, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("test_graph_core.vertex_probe_batch",
		"Use bounded multi-key vertex checks in unordered endpoint sets.", NULL,
		&graph_vertex_probe_batch, true, PGC_USERSET, 0, NULL, NULL, NULL);
	gucs_registered = true;
}

void
GgcUnregisterGraphDiagnostics(const GgcGraphDiagnosticOps *ops)
{
	if (diagnostic_ops != ops)
		return;
	test_config.enabled = false;
	cbo_config.enabled = false;
	diagnostic_ops = NULL;
}

static Datum
test_graph_fixed_config(PG_FUNCTION_ARGS)
{
	Datum	   *edges;
	Datum	   *indexes;
	Datum	   *directions;
	bool	   *edge_nulls;
	bool	   *index_nulls;
	bool	   *direction_nulls;
	int			nedges;
	int			nindexes;
	int			ndirections;
	int			pause_ms = PG_GETARG_INT32(4);
	int			anchor_batch = PG_GETARG_INT32(5);
	int			parent_batch = PG_GETARG_INT32(6);
	int			max_bindings = PG_GETARG_INT32(7);
	MemoryContext old;
	Oid		   *new_edge_relids;
	Oid		   *new_index_relids;
	int32	   *new_directions;

	if (pause_ms < 0 || pause_ms > 15000 || anchor_batch <= 0 ||
		parent_batch <= 0 || max_bindings <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid fixed graph test configuration")));
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(1), REGCLASSOID, sizeof(Oid), true,
		TYPALIGN_INT, &edges, &edge_nulls, &nedges);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(2), REGCLASSOID, sizeof(Oid), true,
		TYPALIGN_INT, &indexes, &index_nulls, &nindexes);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), INT4OID, sizeof(int32), true,
		TYPALIGN_INT, &directions, &direction_nulls, &ndirections);
	if (nedges <= 0 || nedges != nindexes || nedges != ndirections ||
		nedges > PG_INT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed graph stage arrays must have equal nonzero length")));
	for (int i = 0; i < nedges; i++)
	{
		if (edge_nulls[i] || index_nulls[i] || direction_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null fixed graph stage is unsupported")));
		if (DatumGetInt32(directions[i]) != GGC_OUT &&
			DatumGetInt32(directions[i]) != GGC_IN)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid fixed graph stage direction")));
	}
	old = MemoryContextSwitchTo(TopMemoryContext);
	new_edge_relids = palloc(sizeof(Oid) * nedges);
	new_index_relids = palloc(sizeof(Oid) * nedges);
	new_directions = palloc(sizeof(int32) * nedges);
	for (int i = 0; i < nedges; i++)
	{
		new_edge_relids[i] = DatumGetObjectId(edges[i]);
		new_index_relids[i] = DatumGetObjectId(indexes[i]);
		new_directions[i] = DatumGetInt32(directions[i]);
	}
	if (test_config.edge_relids)
		pfree(test_config.edge_relids);
	if (test_config.index_relids)
		pfree(test_config.index_relids);
	if (test_config.fallback_index_relids)
		pfree(test_config.fallback_index_relids);
	if (test_config.directions)
		pfree(test_config.directions);
	test_config.edge_relids = new_edge_relids;
	test_config.index_relids = new_index_relids;
	test_config.fallback_index_relids = NULL;
	test_config.directions = new_directions;
	test_config.enabled = true;
	test_config.request_relid = PG_GETARG_OID(0);
	test_config.stage_count = nedges;
	test_config.pause_ms = pause_ms;
	test_config.wrapper_mode = pause_ms > 0 ? 2 : 0;
	test_config.primary_provider = (pause_ms > 0 ?
		graph_diagnostic_wrapper(graph_index_provider(new_index_relids[0]), 2, pause_ms) :
		graph_index_provider(new_index_relids[0]))->identity;
	test_config.fallback_provider = test_config.primary_provider;
	test_config.primary_build_generation = 0;
	test_config.catalog_generation = 0;
	test_config.statistics_generation = 0;
	test_config.graph_oid = InvalidOid;
	test_config.has_fallback = false;
	test_config.endpoint_domain_is_edge = false;
	test_config.anchor_batch_size = anchor_batch;
	test_config.max_parent_batch = parent_batch;
	test_config.max_bindings = max_bindings;
	test_config.max_endpoints = 8;
	test_config.max_candidates = 16;
	test_config.max_inflight_pages = 4;
	test_config.max_restore_bytes = 1024 * 1024;
	test_config.prefetch = true;
	test_config.preserve_order = true;
	test_config.factor_bytes = 0;
	test_config.endpoint_set = false;
	MemoryContextSwitchTo(old);
	PG_RETURN_INT32(nedges);
}

static Datum
test_graph_fixed_tuning(PG_FUNCTION_ARGS)
{
	uint32 max_endpoints = PG_GETARG_INT32(0);
	uint32 max_candidates = PG_GETARG_INT32(1);
	uint32 max_inflight_pages = PG_GETARG_INT32(2);
	int64 max_restore_bytes = PG_GETARG_INT64(3);

	if (!test_config.enabled)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("configure a fixed graph plan before tuning it")));
	if (max_endpoints == 0 || max_candidates == 0 ||
		max_inflight_pages == 0 || max_restore_bytes <= 0 ||
		max_restore_bytes > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid fixed graph test tuning")));
	test_config.max_endpoints = max_endpoints;
	test_config.max_candidates = max_candidates;
	test_config.max_inflight_pages = max_inflight_pages;
	test_config.max_restore_bytes = (Size) max_restore_bytes;
	test_config.prefetch = PG_GETARG_BOOL(4);
	test_config.preserve_order = PG_GETARG_BOOL(5);
	PG_RETURN_BOOL(true);
}

static Datum
test_graph_fixed_factorize(PG_FUNCTION_ARGS)
{
	int64 bytes = PG_GETARG_INT64(0);

	if (!test_config.enabled || bytes < 0 || bytes > MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid factorized graph test configuration")));
	test_config.factor_bytes = (Size) bytes;
	PG_RETURN_INT64(bytes);
}

/* Diagnostic-only scalar consumer. Explicitly changes the request relation's
 * first output column to a terminal key. Not a production SQL rewrite. */
static Datum
test_graph_fixed_endpoint_set(PG_FUNCTION_ARGS)
{
	if (!test_config.enabled)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("configure a graph plan before endpoint-set mode")));
	test_config.endpoint_set = PG_GETARG_BOOL(0);
	PG_RETURN_BOOL(test_config.endpoint_set);
}

static Datum
test_graph_lifecycle_config(PG_FUNCTION_ARGS)
{
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Datum	   *fallbacks;
	bool	   *fallback_nulls;
	int			nfallbacks;
	int64		build_generation = PG_GETARG_INT64(2);
	int64		catalog_generation = PG_GETARG_INT64(3);
	int64		statistics_generation = PG_GETARG_INT64(4);
	Oid			graph_oid = PG_GETARG_OID(5);
	Oid		   *new_fallbacks = NULL;
	MemoryContext old;

	if (!test_config.enabled)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("configure a fixed graph plan before its lifecycle")));
	if (build_generation < 0 || catalog_generation < 0 ||
		statistics_generation < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("graph lifecycle generations must be nonnegative")));
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(1), REGCLASSOID, sizeof(Oid), true,
		TYPALIGN_INT, &fallbacks, &fallback_nulls, &nfallbacks);
	if (nfallbacks != 0 && nfallbacks != test_config.stage_count)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fallback index count must be zero or equal the stage count")));
	old = MemoryContextSwitchTo(TopMemoryContext);
	if (nfallbacks > 0)
	{
		new_fallbacks = palloc(sizeof(Oid) * nfallbacks);
		for (int i = 0; i < nfallbacks; i++)
		{
			if (fallback_nulls[i])
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("fallback graph index must not be null")));
			new_fallbacks[i] = DatumGetObjectId(fallbacks[i]);
		}
	}
	if (test_config.fallback_index_relids)
		pfree(test_config.fallback_index_relids);
	test_config.fallback_index_relids = new_fallbacks;
	test_config.has_fallback = nfallbacks > 0;
	if (nfallbacks > 0)
		test_config.fallback_provider = graph_index_provider(new_fallbacks[0])->identity;
	test_config.primary_build_generation = (uint64) build_generation;
	test_config.catalog_generation = (uint64) catalog_generation;
	test_config.statistics_generation = (uint64) statistics_generation;
	test_config.graph_oid = graph_oid;
	test_config.pause_ms = 0;
	test_config.wrapper_mode = 0;
	test_config.endpoint_domain_is_edge = false;
	{
		const GgcAdjProviderV2 *provider =
			graph_index_provider(test_config.index_relids[0]);

		test_config.primary_provider = provider->identity;
		if (strcmp(mode, "btree") == 0 || strcmp(mode, "graph_adj") == 0)
		{
			/* Preserve legacy test mode names, but resolve only from this AM. */
			const char *expected = strcmp(mode, "btree") == 0 ? "btree_adj" : mode;

			if (strcmp(NameStr(provider->identity.name), expected) != 0)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("lifecycle mode does not match the index provider")));
		}
		else if (strcmp(mode, "abi_reject") == 0)
			test_config.primary_provider.abi_version = GGC_ADJ_ABI_VERSION + 1;
		else if (strcmp(mode, "runtime_error") == 0 || strcmp(mode, "counted") == 0 ||
				 strcmp(mode, "startup_pause") == 0)
		{
			test_config.wrapper_mode = strcmp(mode, "startup_pause") == 0 ? 6 :
				(strcmp(mode, "counted") == 0 ? 5 : 1);
			if (test_config.wrapper_mode == 6)
				test_config.pause_ms = 3000;
			test_config.primary_provider =
				graph_diagnostic_wrapper(provider, test_config.wrapper_mode, 0)->identity;
		}
		else
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("unknown fixed graph lifecycle mode \"%s\"", mode)));
		test_config.endpoint_domain_is_edge =
			(provider->capabilities & GGC_CAP_HANDLE_INPUT) != 0;
	}
	diagnostic_ops->reset_counters();
	MemoryContextSwitchTo(old);
	PG_RETURN_TEXT_P(cstring_to_text(mode));
}

static Datum
test_graph_lifecycle_diagnostic(PG_FUNCTION_ARGS)
{
	uint64		validate_calls;
	uint64		begin_calls;
	uint64		getnext_calls;
	uint64		end_calls;
	int			active;

	diagnostic_ops->get_counters(&validate_calls, &begin_calls,
		&getnext_calls, &end_calls, &active);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(
		"{\"wrapper_validate\":" UINT64_FORMAT
		",\"wrapper_begin\":" UINT64_FORMAT
		",\"wrapper_getnext\":" UINT64_FORMAT
		",\"wrapper_end\":" UINT64_FORMAT
		",\"wrapper_active\":%d,\"cbo_plan_builds\":" UINT64_FORMAT "}",
		validate_calls, begin_calls, getnext_calls, end_calls, active,
		cbo_diagnostic.plan_builds)));
}

static Datum
test_graph_cbo_config(PG_FUNCTION_ARGS)
{
	Oid			request_relid = PG_GETARG_OID(0);
	Oid			edge_relid = PG_GETARG_OID(1);
	Oid			index_relid = PG_GETARG_OID(2);
	Oid			graph_oid = PG_GETARG_OID(3);
	Oid			edge_element_oid = PG_GETARG_OID(4);
	uint32		endpoint_domain = PG_GETARG_INT32(5);
	uint64		catalog_generation = PG_GETARG_INT64(6);
	uint64		security_generation = PG_GETARG_INT64(7);
	uint64		build_generation = PG_GETARG_INT64(8);
	uint64		fingerprint = PG_GETARG_INT64(9);
	int32		stage_count = PG_GETARG_INT32(10);
	int32		clause_id = PG_GETARG_INT32(11);

	if (stage_count <= 0 || stage_count > PG_INT16_MAX || clause_id < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph CBO test configuration")));
	cbo_config.enabled = true;
	cbo_config.request_relid = request_relid;
	cbo_config.edge_relid = edge_relid;
	cbo_config.index_relid = index_relid;
	cbo_config.stage_count = stage_count;
	cbo_config.predicate_clause_id = clause_id;
	cbo_config.logical_scope = (GgcStatsScope) {
		.graph_oid = graph_oid, .edge_element_oid = edge_element_oid,
		.effective_role = GetUserId(), .endpoint_domain = endpoint_domain,
		.scope_kind = GGC_STATS_LOGICAL, .direction = GGC_OUT,
		.path_mode = 1, .result_mode = 1,
		.catalog_generation = catalog_generation,
		.security_generation = security_generation,
		.semantic_fingerprint = fingerprint
	};
	cbo_config.physical_scope = cbo_config.logical_scope;
	cbo_config.physical_scope.scope_kind = GGC_STATS_PHYSICAL;
	cbo_config.physical_scope.index_relid = index_relid;
	cbo_config.physical_scope.build_generation = build_generation;
	cbo_config.cost_scope = (GgcStatsScope) {
		.effective_role = GetUserId(), .scope_kind = GGC_STATS_COST,
		.semantic_fingerprint = GGC_GRAPH_COST_PROFILE_VERSION
	};
	MemSet(&cbo_diagnostic, 0, sizeof(cbo_diagnostic));
	PG_RETURN_INT32(stage_count);
}

static Datum
test_graph_cbo_diagnostic(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(psprintf(
		"{\"exact_relid_attempts\":" UINT64_FORMAT
		",\"chain_matches\":" UINT64_FORMAT
		",\"contract_accepts\":" UINT64_FORMAT
		",\"paths_retained\":" UINT64_FORMAT
		",\"plan_builds\":" UINT64_FORMAT
		",\"pgq_attempts\":" UINT64_FORMAT
		",\"pgq_security_rejects\":" UINT64_FORMAT
		",\"pgq_contract_rejects\":" UINT64_FORMAT
		",\"pgq_paths_retained\":" UINT64_FORMAT
		",\"available_quals\":%d,\"consumed_quals\":%d"
		",\"qual_shape\":\"%s\",\"reject_reason\":\"%s\",\"startup_cost\":%.17g"
		",\"total_cost\":%.17g}",
		cbo_diagnostic.exact_relid_attempts, cbo_diagnostic.chain_matches,
		cbo_diagnostic.contract_accepts, cbo_diagnostic.paths_retained,
		cbo_diagnostic.plan_builds,
		cbo_diagnostic.pgq_attempts, cbo_diagnostic.pgq_security_rejects,
		cbo_diagnostic.pgq_contract_rejects,
		cbo_diagnostic.pgq_paths_retained,
		cbo_diagnostic.available_quals, cbo_diagnostic.consumed_quals,
		cbo_diagnostic.qual_shape,
		GgcGraphPathRejectReasonName(cbo_diagnostic.reject_reason),
		cbo_diagnostic.startup_cost, cbo_diagnostic.total_cost)));
}

Datum
GgcGraphDiagnosticCall(GgcGraphDiagnosticCallKind kind, FunctionCallInfo fcinfo)
{
	if (!diagnostic_ops)
		elog(ERROR, "graph diagnostics are not registered");
	switch (kind)
	{
		case GGC_DIAGNOSTIC_FIXED_CONFIG: return test_graph_fixed_config(fcinfo);
		case GGC_DIAGNOSTIC_FIXED_TUNING: return test_graph_fixed_tuning(fcinfo);
		case GGC_DIAGNOSTIC_FIXED_FACTORIZE: return test_graph_fixed_factorize(fcinfo);
		case GGC_DIAGNOSTIC_FIXED_ENDPOINT_SET: return test_graph_fixed_endpoint_set(fcinfo);
		case GGC_DIAGNOSTIC_LIFECYCLE_CONFIG: return test_graph_lifecycle_config(fcinfo);
		case GGC_DIAGNOSTIC_LIFECYCLE: return test_graph_lifecycle_diagnostic(fcinfo);
		case GGC_DIAGNOSTIC_CBO_CONFIG: return test_graph_cbo_config(fcinfo);
		case GGC_DIAGNOSTIC_CBO: return test_graph_cbo_diagnostic(fcinfo);
	}
	elog(ERROR, "invalid graph diagnostic operation");
	return (Datum) 0;
}
