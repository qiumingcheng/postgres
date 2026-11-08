/* Fixed PGQ execution: validated plan description, MVCC property access,
 * binding delivery, lifetime management and EXPLAIN. Included by graph_pgq.c;
 * no standalone linker API or planner metadata decoding is duplicated here. */

typedef struct PgqPropertyColumn
{
	int			output;
	AttrNumber attribute;
	bool		typbyval;
	int16		typlen;
	Datum		value;
	bool		isnull;
} PgqPropertyColumn;

/* One same-snapshot scan and optional last-row cache per graph variable. */
typedef struct PgqPropertyScan
{
	Relation	relation;
	Relation	index;
	IndexScanDesc scan;
	TupleTableSlot *slot;
	int			kind;
	int			position;
	uint64		fetches;
	uint64		hits;
	MemoryContext cache_context;
	PgqPropertyColumn *columns;
	int			ncolumns;
	bool		has_byref;
	Size		cache_bytes;
	int64		cached_identity;
	bool		cache_valid;
	bool		identity_seen;
} PgqPropertyScan;

typedef struct PgqLocalQual
{
	ExprState  *expression;
	ExprContext *econtext;
	TupleTableSlot *slot;
	const GgcPgqLocalMapping *mapping;
	uint64		removed;
} PgqLocalQual;

struct PgqFixedScanState;
typedef struct PgqBindingQual
{
	struct PgqFixedScanState *owner;
	ExprState  *expression;
	ExprContext *econtext;
	TupleTableSlot *slot;
	Bitmapset  *columns;
	uint64		removed;
} PgqBindingQual;

typedef struct PgqFixedScanState
{
	CustomScanState css;
	CustomScan *plan;
	GgcPgqPlan *decoded;		/* validated once, owned by query context */
	GgcFixedState *fixed;
	uint32		stage_count;
	int			pause_ms;
	bool		join_mode;
	bool		endpoint_set;
	bool		timing;
	double		fixed_call_us;
	double		result_build_us;
	/* Borrowed scan values, just like a heap slot: valid until next/rescan/end.
	 * Materialize/Sort must copy them before advancing this scan. */
	ArrayType  *array_vertices;
	ArrayType  *array_edges;
	GgcFixedTextArrayState payload_array;
	bool		array_layout_enabled;
	bool		array_fused;
	bool		array_layout_valid;
	uint32	   *array_element_sizes;
	uint64		array_layout_updates;
	uint64		array_layout_rebuilds;
	PgqLocalQual *local_quals;	/* edge, destination vertex, per stage */
	uint32		local_qual_count;
	PgqPropertyScan *properties;
	int			property_count;
	Size		property_cache_budget;
	Size		property_cache_bytes;
	PgqBindingQual *binding_quals;
	uint32		binding_qual_count;
} PgqFixedScanState;

static Node *
pgq_create_fixed_state(CustomScan *cscan)
{
	PgqFixedScanState *state = palloc0(sizeof(*state));

	state->css.ss.ps.type = T_CustomScanState;
	state->css.methods = &pgq_fixed_exec_methods;
	state->plan = cscan;
	/* CreateCustomScanState is the first framework callback.  Reject a bad
	 * scan target before ExecInitCustomScan builds its TupleDesc/projection;
	 * waiting until BeginCustomScan is too late for malformed target Nodes. */
	state->decoded = GgcPgqDecodeFixedPlan(cscan);
	return (Node *) state;
}

/* Both sources are borrowed MVCC-visible rows. A fresh per-tuple context
 * bounds expression allocations; no result is retained across callbacks. */
static bool
pgq_local_qual(PgqLocalQual *qual, const GgcBatchTupleView *edge,
				TupleTableSlot *vertex)
{
	TupleTableSlot *slot = qual->slot;
	MemoryContext old;
	bool accepted;

	CHECK_FOR_INTERRUPTS();
	ResetExprContext(qual->econtext);
	old = MemoryContextSwitchTo(qual->econtext->ecxt_per_tuple_memory);
	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, sizeof(bool) * slot->tts_tupleDescriptor->natts);
	for (int i = 0; i < qual->mapping->count; i++)
	{
		int column = qual->mapping->columns[i].column - 1;
		AttrNumber attribute = qual->mapping->columns[i].attribute;

		slot->tts_values[column] = edge ?
			GgcBatchTupleViewGetAttr(edge, attribute, &slot->tts_isnull[column]) :
			slot_getattr(vertex, attribute, &slot->tts_isnull[column]);
	}
	ExecStoreVirtualTuple(slot);
	qual->econtext->ecxt_scantuple = slot;
	accepted = ExecQual(qual->expression, qual->econtext);
	if (!accepted)
		qual->removed++;
	ExecClearTuple(slot);
	MemoryContextSwitchTo(old);
	return accepted;
}

static bool
pgq_local_edge_qual(void *arg, const GgcBatchTupleView *edge)
{
	return pgq_local_qual(arg, edge, NULL);
}

static bool
pgq_local_vertex_qual(void *arg, TupleTableSlot *vertex)
{
	return pgq_local_qual(arg, NULL, vertex);
}

static void
init_local_quals(PgqFixedScanState *state, EState *estate)
{
	CustomScanState *node = &state->css;
	List *expressions = state->plan->custom_exprs;
	GgcFixedLocalFilter *filters = palloc0(sizeof(*filters) * state->stage_count);
	TupleTableSlot *slot = ExecInitExtraTupleSlot(estate,
		node->ss.ss_ScanTupleSlot->tts_tupleDescriptor, &TTSOpsVirtual);
	ExprContext *econtext = CreateExprContext(estate);

	Assert(!state->endpoint_set);
	state->local_quals = palloc0(sizeof(*state->local_quals) * 2 * state->stage_count);
	for (uint32 i = 0; i < 2 * state->stage_count; i++)
	{
		PgqLocalQual *qual = &state->local_quals[i];
		List *exprs = list_nth(expressions, i);

		if (exprs == NIL)
			continue;
		qual->expression = ExecInitQual(exprs, &node->ss.ps);
		qual->econtext = econtext;
		qual->slot = slot;
		qual->mapping = &state->decoded->locals[i];
		state->local_qual_count += list_length(exprs);
		if (i % 2 == 0)
		{
			filters[i / 2].edge = pgq_local_edge_qual;
			filters[i / 2].edge_arg = qual;
			for (int j = 0; j < qual->mapping->count; j++)
				filters[i / 2].edge_columns = bms_add_member(
					filters[i / 2].edge_columns, qual->mapping->columns[j].attribute);
		}
		else
		{
			filters[i / 2].vertex = pgq_local_vertex_qual;
			filters[i / 2].vertex_arg = qual;
			for (int j = 0; j < qual->mapping->count; j++)
				filters[i / 2].vertex_columns = bms_add_member(
					filters[i / 2].vertex_columns, qual->mapping->columns[j].attribute);
		}
	}
	GgcFixedSetLocalFilters(state->fixed, filters, state->stage_count);
	for (uint32 i = 0; i < state->stage_count; i++)
	{
		bms_free(filters[i].edge_columns);
		bms_free(filters[i].vertex_columns);
	}
	pfree(filters);
}

static void
init_property_scans(PgqFixedScanState *state, EState *estate)
{
	const GgcPgqPlan *decoded = state->decoded;

	state->property_count = decoded->property_count;
	/* A separate, small optional budget; provider and binding workspaces
	 * retain their own explicit limits.  This is not a total RSS limit. */
	state->property_cache_budget = mul_size((Size) work_mem, 1024) / 8;
	state->properties = palloc0(sizeof(*state->properties) * state->property_count);
	for (int position = 0; position < state->property_count; position++)
	{
		const GgcPgqPropertySpec *spec = &decoded->properties[position];
		PgqPropertyScan *property = &state->properties[position];
		Oid relid = spec->relation;
		Oid indexid = spec->index;
		AttrNumber keyatt = spec->key_attribute;

		property->kind = spec->kind;
		property->position = spec->position;
		property->relation = table_open(relid, AccessShareLock);
		property->index = index_open(indexid, AccessShareLock);
		if (state->endpoint_set ||
			property->relation->rd_rel->relkind != RELKIND_RELATION ||
			property->relation->rd_rel->relrowsecurity ||
			property->relation->rd_rel->relhassubclass ||
			property->index->rd_rel->relam != BTREE_AM_OID ||
			property->index->rd_index->indrelid != relid ||
			!property->index->rd_index->indisunique ||
			!property->index->rd_index->indisvalid ||
			!property->index->rd_index->indisready ||
			!property->index->rd_index->indimmediate ||
			property->index->rd_index->indnkeyatts != 1 ||
			property->index->rd_index->indkey.values[0] != keyatt ||
			RelationGetIndexPredicate(property->index) != NIL ||
			RelationGetIndexExpressions(property->index) != NIL ||
			!GgcIntegerKeyType(property->index->rd_opcintype[0]) ||
			property->index->rd_opcintype[0] != get_atttype(relid, keyatt) ||
			get_opfamily_proc(property->index->rd_opfamily[0], INT8OID, INT8OID,
				BTORDER_PROC) != F_BTINT8CMP)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("graph property lookup identity is no longer valid")));
		{
			Bitmapset *columns = bms_make_singleton(keyatt);

			for (int i = 0; i < decoded->output_count; i++)
				if (decoded->outputs[i].kind == GGC_PGQ_OUTPUT_PROPERTY &&
					decoded->outputs[i].position == position)
					columns = bms_add_member(columns, decoded->outputs[i].attribute);
			if (!GgcSelectColumnsAllowed(relid, columns, GetUserId()))
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE,
					RelationGetRelationName(property->relation));
			bms_free(columns);
		}
		property->slot = table_slot_create(property->relation, &estate->es_tupleTable);
		property->scan = index_beginscan(property->relation, property->index,
			estate->es_snapshot, NULL, 1, 0, 0);
		for (int i = 0; i < decoded->output_count; i++)
			if (decoded->outputs[i].kind == GGC_PGQ_OUTPUT_PROPERTY &&
				decoded->outputs[i].position == position)
				property->ncolumns++;
		property->columns = palloc0(sizeof(*property->columns) * property->ncolumns);
		{
			int n = 0;

			for (int output = 0; output < decoded->output_count; output++)
			{
				const GgcPgqOutputMapping *mapping = &decoded->outputs[output];

				if (mapping->kind == GGC_PGQ_OUTPUT_PROPERTY && mapping->position == position)
				{
					PgqPropertyColumn *column = &property->columns[n++];

					column->output = output;
					column->attribute = mapping->attribute;
					column->typbyval = mapping->typbyval;
					column->typlen = mapping->typlen;
					property->has_byref |= !column->typbyval;
				}
			}
		}
	}
}

static void
pgq_reset_property_cache(PgqFixedScanState *state, PgqPropertyScan *property)
{
	Assert(state->property_cache_bytes >= property->cache_bytes);
	state->property_cache_bytes -= property->cache_bytes;
	property->cache_bytes = 0;
	property->cache_valid = false;
	property->identity_seen = false;
	if (property->cache_context)
	{
		MemoryContextDelete(property->cache_context);
		property->cache_context = NULL;
	}
}

/* Cache only explicitly requested columns, never a borrowed heap tuple, pin,
 * or expression result.  A too-wide row simply bypasses this optional cache.
 * The unique identity, relation, role and snapshot are fixed by this scan;
 * ReScan invalidates every entry, including command-ID/snapshot changes. */
static void
pgq_cache_property(PgqFixedScanState *state, PgqPropertyScan *property)
{
	Size available;
	Size bytes = 0;
	MemoryContext old;

	Assert(state->property_cache_bytes <= state->property_cache_budget);
	available = state->property_cache_budget - state->property_cache_bytes;
	Assert(property->identity_seen);
	for (int i = 0; i < property->ncolumns; i++)
	{
		PgqPropertyColumn *column = &property->columns[i];

		column->value = slot_getattr(property->slot, column->attribute, &column->isnull);
		if (!column->isnull && !column->typbyval)
		{
			Size size = datumGetSize(column->value, false, column->typlen);

			if (size > available || bytes > available - size)
				return;
			bytes += size;
		}
	}
	if (bytes)
	{
		/* Leave room for allocator headers/blocks before retaining Datums.
		 * Actual context space, not only payload size, is charged below. */
		if (bytes > available / 2 || available < ALLOCSET_SMALL_INITSIZE)
			return;
		property->cache_context = AllocSetContextCreate(
			state->css.ss.ps.state->es_query_cxt, "graph property cache",
			ALLOCSET_SMALL_SIZES);
		old = MemoryContextSwitchTo(property->cache_context);
		for (int i = 0; i < property->ncolumns; i++)
		{
			PgqPropertyColumn *column = &property->columns[i];

			if (!column->isnull && !column->typbyval)
				column->value = datumCopy(column->value, false, column->typlen);
		}
		MemoryContextSwitchTo(old);
		bytes = MemoryContextMemAllocated(property->cache_context, true);
		if (bytes > available)
		{
			MemoryContextDelete(property->cache_context);
			property->cache_context = NULL;
			return;
		}
	}
	property->cache_bytes = bytes;
	state->property_cache_bytes += bytes;
	property->cache_valid = true;
}

/* Values are owned by the caller's tuple context before the row is unpinned. */
static void
fill_property_slots(PgqFixedScanState *state, const GgcFixedResult *result,
					TupleTableSlot *slot, Bitmapset *needed)
{
	for (int i = 0; i < state->property_count; i++)
	{
		PgqPropertyScan *property = &state->properties[i];
		ScanKeyData key;
		int64 identity;
		bool required = needed == NULL;
		bool cached;

		for (int j = 0; !required && j < property->ncolumns; j++)
			required = bms_is_member(property->columns[j].output, needed);
		if (!required)
			continue;
		if (property->position >= result->hop_count +
			(property->kind == GGC_PGQ_OUTPUT_VERTEX))
			elog(ERROR, "graph predicate requested an unbound property");
		identity = property->kind == GGC_PGQ_OUTPUT_VERTEX ?
			result->vertices[property->position] : result->edges[property->position];

		CHECK_FOR_INTERRUPTS();
		cached = property->cache_valid && property->cached_identity == identity;
		if (cached)
			property->hits++;
		else
		{
			bool repeated = property->identity_seen && property->cached_identity == identity;

			if (property->cache_valid)
				pgq_reset_property_cache(state, property);
			Assert(property->cache_context == NULL && property->cache_bytes == 0);
			GgcIntegerScanKey(&key, property->index->rd_opcintype[0],
			BTEqualStrategyNumber, Int64GetDatum(identity));
			index_rescan(property->scan, &key, 1, NULL, 0);
			if (!index_getnext_slot(property->scan, ForwardScanDirection, property->slot) ||
				property->scan->xs_recheck)
				elog(ERROR, "graph property binding has no exact visible row");
			property->fetches++;
			property->cached_identity = identity;
			property->identity_seen = true;
			/* No attribute extraction, size calculation, or cache allocation
			 * for a new identity with by-reference columns.  Promote only on
			 * observed reuse; fixed-width by-value columns remain immediate.
			 * NULL by-reference columns use the same conservative policy. */
			if (repeated || !property->has_byref)
				pgq_cache_property(state, property);
		}
		for (int j = 0; j < property->ncolumns; j++)
		{
			PgqPropertyColumn *column = &property->columns[j];
			Datum value;

			if (needed && !bms_is_member(column->output, needed))
				continue;
			if (property->cache_valid)
			{
				value = column->value;
				slot->tts_isnull[column->output] = column->isnull;
			}
			else
				value = slot_getattr(property->slot, column->attribute,
					&slot->tts_isnull[column->output]);
			if (!slot->tts_isnull[column->output])
				slot->tts_values[column->output] = datumCopy(value,
					column->typbyval, column->typlen);
		}
		if (!cached)
		{
			ExecClearTuple(property->slot);
			index_rescan(property->scan, NULL, 1, NULL, 0);
		}
	}
}

/* The same mapping populates final output and an early predicate's subset.
 * Future bindings must never be read from a partial prefix. */
static void
pgq_fill_binding_slot(PgqFixedScanState *state, const GgcFixedResult *result,
					  TupleTableSlot *slot, Bitmapset *needed)
{
	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, sizeof(bool) * slot->tts_tupleDescriptor->natts);
	for (int column = 0; column < state->decoded->output_count; column++)
	{
		const GgcPgqOutputMapping *mapping = &state->decoded->outputs[column];
		GgcPgqOutputKind kind = mapping->kind;
		int position = mapping->position;

		if (needed && !bms_is_member(column, needed))
			continue;
		if (kind != GGC_PGQ_OUTPUT_PROPERTY && kind != GGC_PGQ_OUTPUT_FILTER_ONLY &&
			position >= result->hop_count + (kind == GGC_PGQ_OUTPUT_VERTEX))
			elog(ERROR, "graph predicate requested an unbound value");
		switch (kind)
		{
			case GGC_PGQ_OUTPUT_PROPERTY:
			case GGC_PGQ_OUTPUT_FILTER_ONLY:
				break;
			case GGC_PGQ_OUTPUT_VERTEX:
			case GGC_PGQ_OUTPUT_EDGE:
				slot->tts_values[column] = GgcIntegerKeyResult(
					kind == GGC_PGQ_OUTPUT_VERTEX ? result->vertices[position] : result->edges[position],
					TupleDescAttr(slot->tts_tupleDescriptor, column)->atttypid);
				slot->tts_isnull[column] = false;
				break;
			case GGC_PGQ_OUTPUT_PAYLOAD:
				slot->tts_isnull[column] = result->payload_nulls[position];
				if (!slot->tts_isnull[column])
					slot->tts_values[column] = datumCopy(result->payloads[position],
						mapping->typbyval, mapping->typlen);
				break;
			default:
				elog(ERROR, "invalid GraphExpandFixed output mapping");
		}
	}
	fill_property_slots(state, result, slot, needed);
	ExecStoreVirtualTuple(slot);
}

static bool
pgq_binding_qual(void *arg, const GgcFixedResult *binding,
				 const GgcBatchTupleView *candidate)
{
	PgqBindingQual *qual = arg;
	MemoryContext old;
	bool accepted;

	CHECK_FOR_INTERRUPTS();
	ResetExprContext(qual->econtext);
	old = MemoryContextSwitchTo(qual->econtext->ecxt_per_tuple_memory);
	if (qual->columns)
		pgq_fill_binding_slot(qual->owner, binding, qual->slot, qual->columns);
	else
	{
		ExecClearTuple(qual->slot);
		ExecStoreVirtualTuple(qual->slot);
	}
	qual->econtext->ecxt_scantuple = qual->slot;
	accepted = ExecQual(qual->expression, qual->econtext);
	if (!accepted)
		qual->removed++;
	ExecClearTuple(qual->slot);
	MemoryContextSwitchTo(old);
	return accepted;
}

static void
pgq_init_binding_quals(PgqFixedScanState *state, EState *estate)
{
	List *expressions = state->plan->custom_exprs;
	GgcFixedBindingFilter *filters;
	TupleTableSlot *slot;
	ExprContext *econtext;

	if (list_length(expressions) != 3 * state->stage_count)
		return;
	if (state->endpoint_set)
		elog(ERROR, "endpoint sets cannot discard predicate history");
	filters = palloc0(sizeof(*filters) * state->stage_count);
	state->binding_quals = palloc0(sizeof(*state->binding_quals) * state->stage_count);
	slot = ExecInitExtraTupleSlot(estate,
		state->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor, &TTSOpsVirtual);
	econtext = CreateExprContext(estate);
	for (uint32 i = 0; i < state->stage_count; i++)
	{
		PgqBindingQual *qual = &state->binding_quals[i];
		List *exprs = list_nth(expressions, 2 * state->stage_count + i);

		if (exprs == NIL)
			continue;
		qual->owner = state;
		qual->expression = ExecInitQual(exprs, &state->css.ss.ps);
		qual->slot = slot;
		qual->econtext = econtext;
		foreach_node(Var, var, pull_var_clause((Node *) exprs, 0))
		{
			if (var->varno != INDEX_VAR || var->varattno <= 0 ||
				var->varattno > slot->tts_tupleDescriptor->natts)
				elog(ERROR, "graph predicate lost its scan column");
			qual->columns = bms_add_member(qual->columns, var->varattno - 1);
		}
		state->binding_qual_count += list_length(exprs);
		filters[i].accept = pgq_binding_qual;
		filters[i].arg = qual;
	}
	if (state->binding_qual_count)
		GgcFixedSetBindingFilters(state->fixed, filters, state->stage_count);
	pfree(filters);
}

static void
pgq_begin_fixed(CustomScanState *node, EState *estate, int eflags)
{
	PgqFixedScanState *state = (PgqFixedScanState *) node;
	GgcPgqPlan *decoded = state->decoded;
	GgcFixedStagePlan *stages = decoded->stages;
	GgcFixedExecOptions options = decoded->options;
	Plan	   *subplan;

	/* Metadata was validated before the framework initialized scan slots.
	 * Only live provider/catalog/permission checks remain at execution start. */
	state->join_mode = decoded->join_mode;
	state->endpoint_set = decoded->endpoint_set;
	state->stage_count = decoded->stage_count;
	state->pause_ms = decoded->pause_ms;
	subplan = (Plan *) linitial(state->plan->custom_plans);
	node->ss.ps.lefttree = ExecInitNode(subplan, estate, eflags);
	state->timing = (estate->es_instrument & INSTRUMENT_TIMER) != 0;
	if (state->endpoint_set &&
		node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts != 1)
		elog(ERROR, "graph endpoint sets require one scalar target");
	if (decoded->wrapper_mode != 0)
		(void) graph_diagnostic_wrapper(
			graph_index_provider(stages[0].primary.index_relid),
			decoded->wrapper_mode, state->pause_ms);
	options.column_acl = state->join_mode && !enable_graph_age_adjacency &&
		(graph_index_provider(stages[0].primary.index_relid)->capabilities & GGC_CAP_COLUMN_ACL);
	/* This test frontend declares a configurable request budget for PGQ.
	 * The core setter never silently raises a caller-supplied hard limit.
	 * Explicit legacy fixed-executor fixtures retain their own budgets. */
	if (state->endpoint_set && state->join_mode)
	{
		options.max_parent_batch = Max(options.max_parent_batch, graph_endpoint_batch_size);
		options.batch_budget.max_endpoints = graph_endpoint_batch_size;
	}
	if (enable_graph_age_adjacency && !state->endpoint_set)
		elog(ERROR, "visible adjacency requires endpoint-set WALK; bag/path execution is unsupported");
	/* Prepared plans may have been made with the optional AGE route off.
	 * B-tree direction support must not silently widen that route's audited
	 * PGQ admission. The lower-level provider API is unchanged. */
	if (enable_graph_age_adjacency && state->join_mode)
		for (uint32 i = 0; i < state->stage_count; i++)
			if (stages[i].direction != GGC_OUT || decoded->has_reverse)
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("AGE adjacency currently requires all-OUT PGQ stages")));
	state->fixed = GgcFixedBegin(node->ss.ps.lefttree, estate, stages,
		state->stage_count, &options);
	if (decoded->has_reverse)
		GgcFixedSetUndirectedStages(state->fixed, decoded->reverse_access, state->stage_count);
	if (decoded->join.has_binding_positions)
	{
		bool needed = false;

		for (uint32 i = 0; i < state->stage_count; i++)
			needed |= decoded->join.input_positions[i] != i ||
				decoded->join.match_positions[i] >= 0;
		if (needed)
			GgcFixedSetBindingPositions(state->fixed, decoded->join.input_positions,
				decoded->join.match_positions, state->stage_count);
	}
	if (state->plan->custom_exprs)
		init_local_quals(state, estate);
	if (state->join_mode)
		init_property_scans(state, estate);
	pgq_init_binding_quals(state, estate);
	if (state->endpoint_set && node->ss.ps.qual)
		elog(ERROR, "endpoint sets cannot discard predicate history");
	if ((!state->join_mode || state->endpoint_set) && decoded->has_factor_budget)
	{
		Size bytes = decoded->factor_bytes;

		/* Like native work_mem consumers, prepared queries use the executor's
		 * current limit, not the value captured when the plan was created. */
		if (state->join_mode && state->endpoint_set)
			bytes = mul_size((Size) work_mem, 1024);
		if (state->endpoint_set)
		{
			if (!bytes || !GgcFixedEnableEndpointSet(state->fixed, bytes))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("ineligible graph endpoint-set request")));
			GgcFixedSetProbeBatching(state->fixed, graph_edge_probe_batch,
				graph_vertex_probe_batch);
			GgcFixedSetEndpointPipeline(state->fixed, graph_endpoint_pipeline,
				state->join_mode ? graph_endpoint_batch_size : 32, graph_vertex_batch_size);
			GgcFixedSetRangeProbes(state->fixed, graph_edge_range_probe, graph_vertex_range_probe);
			GgcFixedSetVertexPointerSort(state->fixed, graph_vertex_pointer_sort);
			GgcFixedSetBtreeSeekHint(state->fixed, graph_btree_seek_hint);
			GgcFixedSetBtreeBatchOptions(state->fixed, graph_btree_batch_route,
									   graph_btree_int8_compare);
			if (enable_graph_age_adjacency)
				GgcFixedUseVisibleAdj(state->fixed);
			GgcFixedSetEdgeIndexOnly(state->fixed, graph_edge_index_only);
			return;
		}
		if (bytes)
			(void) GgcFixedEnableFactorized(state->fixed, bytes);
	}
	if (!state->join_mode)
	{
		Datum *zero_values;

		state->payload_array.array_context = estate->es_query_cxt;
		state->array_layout_enabled = GgcFixedIsFactorized(state->fixed);
		state->array_fused = GgcFixedEnableArrayOutput(state->fixed,
			node->ss.ss_ScanTupleSlot, node->ss.ps.ps_ExprContext);
		if (state->array_fused)
			return;
		zero_values = palloc0(sizeof(Datum) * (state->stage_count + 1));
		state->array_vertices = construct_array(zero_values, state->stage_count + 1,
			INT8OID, sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
		state->array_edges = construct_array(zero_values, state->stage_count,
			INT8OID, sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
		state->payload_array.array_payloads = palloc(sizeof(Datum) * state->stage_count);
		pfree(zero_values);
	}
}

/* The diagnostic interface's payload column is text[].  Copy packed text
 * directly into its flat array representation, avoiding a detoast allocation
 * for every short varlena in every result.  External/compressed values still
 * go through the normal detoaster, in the caller's per-tuple context. */

/* Reuse a non-NULL flat array's layout, never its values. Every changed
 * payload is checked for NULL, external/compressed representation and exact
 * length before copying actual bytes. No heap/DAG pointer is cached here.
 * Changing sizes or NULLs falls back to the original full array builder. */
static ArrayType *
build_layout_payload_array(PgqFixedScanState *state, const GgcFixedResult *result)
{
	uint32 last = result->hop_count - 1;
	bool new_prefix = !state->payload_array.array_prefix_valid ||
		state->payload_array.array_prefix_ordinal != result->prefix_ordinal;
	ArrayType *array = state->payload_array.array_payload_buffer;
	char *dest;

	if (state->array_layout_valid)
	{
		dest = ARR_DATA_PTR(array) + (new_prefix ? 0 : state->payload_array.array_prefix_bytes);
		for (uint32 i = new_prefix ? 0 : last; i < result->hop_count; i++)
		{
			struct varlena *value;
			Size length;

			if (result->payload_nulls[i])
				goto rebuild;
			value = (struct varlena *) DatumGetPointer(result->payloads[i]);
			if (VARATT_IS_EXTERNAL(value) || VARATT_IS_COMPRESSED(value))
				goto rebuild;
			length = VARSIZE_ANY_EXHDR(value);
			if (length + VARHDRSZ != state->array_element_sizes[i])
				goto rebuild;
			memcpy(dest + VARHDRSZ, VARDATA_ANY(value), length);
			dest += INTALIGN(state->array_element_sizes[i]);
		}
		state->payload_array.array_prefix_valid = true;
		state->payload_array.array_prefix_ordinal = result->prefix_ordinal;
		state->array_layout_updates++;
		return array;
	}

rebuild:
	/* Earlier fields may already contain the new row. Force a full rebuild
	 * on a rejected in-place attempt; reusing the old prefix would be unsafe. */
	if (state->array_layout_valid)
		state->payload_array.array_prefix_valid = false;
	state->array_layout_valid = false;
	array = GgcFixedBuildPayloadArray(&state->payload_array, result);
	state->array_layout_rebuilds++;
	if (!ARR_HASNULL(array))
	{
		if (!state->array_element_sizes)
			state->array_element_sizes = MemoryContextAlloc(state->payload_array.array_context,
				mul_size(result->hop_count, sizeof(uint32)));
		dest = ARR_DATA_PTR(array);
		for (uint32 i = 0; i < result->hop_count; i++)
		{
			state->array_element_sizes[i] = VARSIZE(dest);
			dest += INTALIGN(state->array_element_sizes[i]);
		}
		state->array_layout_valid = true;
	}
	return array;
}

static TupleTableSlot *
pgq_exec_fixed(CustomScanState *node)
{
	PgqFixedScanState *state = (PgqFixedScanState *) node;
	GgcFixedResult result;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	MemoryContext old;

	instr_time	started;
	instr_time	finished;
	bool		found;
	int64		endpoint_anchor = 0;
	int64		endpoint = 0;

next_binding:
	CHECK_FOR_INTERRUPTS();
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	if (state->endpoint_set)
		found = GgcFixedNextEndpoint(state->fixed, &endpoint_anchor, &endpoint);
	else
		found = GgcFixedNext(state->fixed, &result);
	if (state->timing)
	{
		INSTR_TIME_SET_CURRENT_FAST(finished);
		INSTR_TIME_SUBTRACT(finished, started);
		state->fixed_call_us += INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
	}
	if (!found)
		return NULL;
	if (state->endpoint_set)
	{
		ResetExprContext(econtext);
		ExecClearTuple(slot);
		slot->tts_values[0] = GgcIntegerKeyResult(endpoint,
			TupleDescAttr(slot->tts_tupleDescriptor, 0)->atttypid);
		slot->tts_isnull[0] = false;
		ExecStoreVirtualTuple(slot);
		econtext->ecxt_scantuple = slot;
		return node->ss.ps.ps_ProjInfo ? ExecProject(node->ss.ps.ps_ProjInfo) : slot;
	}
	if (state->array_fused)
	{
		/* Next has reset the per-tuple context and filled final owned arrays.
		 * Projection still executes exactly once, after result production. */
		if (node->ss.ps.ps_ProjInfo)
			slot = ExecProject(node->ss.ps.ps_ProjInfo);
		return slot;
	}
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	ResetExprContext(econtext);
	old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	if (state->join_mode)
	{
		pgq_fill_binding_slot(state, &result, slot, NULL);
		econtext->ecxt_scantuple = slot;
		MemoryContextSwitchTo(old);
		if (!ExecQual(node->ss.ps.qual, econtext))
		{
			InstrCountFiltered1(node, 1);
			ExecClearTuple(slot);
			if (state->timing)
			{
				INSTR_TIME_SET_CURRENT_FAST(finished);
				INSTR_TIME_SUBTRACT(finished, started);
				state->result_build_us += INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
			}
			goto next_binding;
		}
		if (node->ss.ps.ps_ProjInfo)
			slot = ExecProject(node->ss.ps.ps_ProjInfo);
		if (state->timing)
		{
			INSTR_TIME_SET_CURRENT_FAST(finished);
			INSTR_TIME_SUBTRACT(finished, started);
			state->result_build_us += INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
		}
		return slot;
	}
	ExecClearTuple(slot);
	memcpy(ARR_DATA_PTR(state->array_vertices), result.vertices,
		   sizeof(int64) * (result.hop_count + 1));
	memcpy(ARR_DATA_PTR(state->array_edges), result.edges,
		   sizeof(int64) * result.hop_count);
	slot->tts_values[0] = Int64GetDatum(result.vertices[0]);
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = PointerGetDatum(state->array_vertices);
	slot->tts_isnull[1] = false;
	slot->tts_values[2] = PointerGetDatum(state->array_edges);
	slot->tts_isnull[2] = false;
	slot->tts_values[3] = PointerGetDatum(state->array_layout_enabled ?
		build_layout_payload_array(state, &result) : GgcFixedBuildPayloadArray(&state->payload_array, &result));
	slot->tts_isnull[3] = false;
	ExecStoreVirtualTuple(slot);
	econtext->ecxt_scantuple = slot;
	MemoryContextSwitchTo(old);
	if (node->ss.ps.ps_ProjInfo)
		slot = ExecProject(node->ss.ps.ps_ProjInfo);
	if (state->timing)
	{
		INSTR_TIME_SET_CURRENT_FAST(finished);
		INSTR_TIME_SUBTRACT(finished, started);
		state->result_build_us += INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
	}
	return slot;
}

static void
pgq_end_fixed(CustomScanState *node)
{
	PgqFixedScanState *state = (PgqFixedScanState *) node;

	GgcFixedEnd(state->fixed);
	state->fixed = NULL;
	for (int i = 0; i < state->property_count; i++)
	{
		PgqPropertyScan *property = &state->properties[i];

		pgq_reset_property_cache(state, property);
		ExecClearTuple(property->slot);
		index_endscan(property->scan);
		index_close(property->index, NoLock);
		table_close(property->relation, NoLock);
	}
	ExecEndNode(node->ss.ps.lefttree);
}

static void
pgq_rescan_fixed(CustomScanState *node)
{
	PgqFixedScanState *state = (PgqFixedScanState *) node;

	for (int i = 0; i < state->property_count; i++)
		pgq_reset_property_cache(state, &state->properties[i]);
	GgcFixedRescan(state->fixed);
	state->payload_array.array_prefix_valid = false;
	state->array_layout_valid = false;
}

static void
pgq_explain_fixed(CustomScanState *node, List *ancestors, ExplainState *es)
{
	PgqFixedScanState *state = (PgqFixedScanState *) node;
	const GgcPgqJoinMetadata *meta = &state->decoded->join;
	const char *logical;
	const char *physical;
	const char *cost;

	ExplainPropertyInteger("Graph Stages", NULL, state->stage_count, es);
	ExplainPropertyText("Graph Stage Model", "generic fixed-N loop", es);
	ExplainPropertyText("Graph Batch Pipeline", "endpoint + candidate/heap-page", es);
	if (meta->has_binding_positions)
	{
		StringInfoData inputs;
		StringInfoData matches;
		uint64 rejections = 0;

		initStringInfo(&inputs);
		initStringInfo(&matches);
		for (uint32 i = 0; i < state->stage_count; i++)
		{
			appendStringInfo(&inputs, "%s%u", i ? ", " : "", meta->input_positions[i]);
			appendStringInfo(&matches, "%s%d", i ? ", " : "", meta->match_positions[i]);
			rejections += GgcFixedGetBoundKeyRejections(state->fixed, i);
		}
		ExplainPropertyInteger("Graph Anchor Position", NULL, meta->anchor_position, es);
		ExplainPropertyInteger("Graph Plan Candidates", NULL, meta->candidates, es);
		ExplainPropertyInteger("Graph Candidate Budget", NULL, meta->candidate_budget, es);
		ExplainPropertyBool("Graph Enumeration Limited", meta->enumeration_limited, es);
		ExplainPropertyText("Graph Input Positions", inputs.data, es);
		ExplainPropertyText("Graph Bound-Key Positions", matches.data, es);
		if (es->analyze)
			ExplainPropertyUInteger("Graph Bound-Key Rejections", NULL, rejections, es);
		pfree(inputs.data);
		pfree(matches.data);
	}
	{
		StringInfoData directions;
		StringInfoData indexes;
		bool extended = false;

		initStringInfo(&directions);
		initStringInfo(&indexes);
		for (uint32 i = 0; i < state->stage_count; i++)
		{
			const GgcFixedStagePlan *stage = &state->decoded->stages[i];
			Oid reverse = state->decoded->reverse_access[i].index_relid;
			bool undirected = OidIsValid(reverse);
			bool incoming = stage->direction == GGC_IN;

			if (i)
			{
				appendStringInfoString(&directions, ", ");
				appendStringInfoString(&indexes, ", ");
			}
			appendStringInfoString(&directions, undirected ? "UNDIRECTED" : incoming ? "IN" : "OUT");
			appendStringInfoString(&indexes, get_rel_name(stage->primary.index_relid));
			if (undirected)
				appendStringInfo(&indexes, "+%s", get_rel_name(reverse));
			extended |= undirected || incoming;
		}
		if (extended)
		{
			ExplainPropertyText("Graph Stage Directions", directions.data, es);
			ExplainPropertyText("Graph Stage Indexes", indexes.data, es);
		}
		pfree(directions.data);
		pfree(indexes.data);
	}
	if (state->plan->scan.plan.qual || state->binding_qual_count)
		ExplainPropertyInteger("Graph Binding Filters", NULL,
			list_length(state->plan->scan.plan.qual) + state->binding_qual_count, es);
	if (state->binding_qual_count)
	{
		uint64 removed = 0;

		ExplainPropertyInteger("Graph Early Binding Filters", NULL, state->binding_qual_count, es);
		for (uint32 i = 0; i < state->stage_count; i++)
			removed += state->binding_quals[i].removed;
		if (es->analyze)
			ExplainPropertyUInteger("Graph Early Binding Rows Removed", NULL, removed, es);
	}
	if (state->property_count)
	{
		uint64 fetches = 0;
		uint64 hits = 0;

		for (int i = 0; i < state->property_count; i++)
		{
			fetches += state->properties[i].fetches;
			hits += state->properties[i].hits;
		}
		ExplainPropertyInteger("Graph Property Scans", NULL, state->property_count, es);
		if (es->analyze)
		{
			ExplainPropertyUInteger("Graph Property Fetches", NULL, fetches, es);
			ExplainPropertyUInteger("Graph Property Cache Hits", NULL, hits, es);
		}
	}
	if (state->local_qual_count)
	{
		uint64 removed = 0;

		ExplainPropertyInteger("Graph Local Filters", NULL, state->local_qual_count, es);
		if (es->analyze)
		{
			for (uint32 i = 0; i < 2 * state->stage_count; i++)
				removed += state->local_quals[i].removed;
			ExplainPropertyInteger("Graph Local Filter Rows Removed", NULL, removed, es);
		}
	}
	if (GgcFixedVisibleAdjName(state->fixed))
		ExplainPropertyText("Graph Visible Adjacency", GgcFixedVisibleAdjName(state->fixed), es);
	if (GgcFixedIsFactorized(state->fixed))
	{
		ExplainPropertyText("Graph Prefix Representation", state->endpoint_set ?
			"exact-k WALK endpoint sets (unordered)" : "factorized WALK (unordered)", es);
		ExplainPropertyInteger("Factorized Peak Bytes", NULL,
			GgcFixedFactorizedPeakBytes(state->fixed), es);
		ExplainPropertyInteger("Factorized Arcs", NULL,
			GgcFixedFactorizedArcs(state->fixed), es);
		if (es->analyze)
		{
			if (state->array_fused)
				GgcFixedGetArrayOutputStats(state->fixed,
					&state->array_layout_updates, &state->array_layout_rebuilds);
			ExplainPropertyBool("Fused Array Output", state->array_fused, es);
			ExplainPropertyInteger("Payload Layout Updates", NULL,
				state->array_layout_updates, es);
			ExplainPropertyInteger("Payload Layout Rebuilds", NULL,
				state->array_layout_rebuilds, es);
		}
	}
	if (es->analyze && state->timing)
		ExplainPropertyText("Graph Delivery Timing",
			psprintf("fixed_call_us=%.3f result_build_us=%.3f",
				state->fixed_call_us, state->result_build_us), es);
	if (state->join_mode)
	{
		switch (meta->logical_state)
		{
			case GGC_STATS_READY: logical = "READY"; break;
			case GGC_STATS_PARTIAL: logical = "PARTIAL"; break;
			case GGC_STATS_STALE: logical = "STALE"; break;
			default: logical = "MISSING"; break;
		}
		switch (meta->physical_state)
		{
			case GGC_STATS_READY: physical = "READY"; break;
			case GGC_STATS_PARTIAL: physical = "PARTIAL"; break;
			case GGC_STATS_STALE: physical = "STALE"; break;
			default: physical = "MISSING"; break;
		}
		switch (meta->cost_state)
		{
			case GGC_STATS_READY: cost = "READY"; break;
			case GGC_STATS_PARTIAL: cost = "PARTIAL"; break;
			case GGC_STATS_STALE: cost = "STALE"; break;
			default: cost = "MISSING"; break;
		}
		ExplainPropertyText("Graph Statistics",
			psprintf("logical=%s generation=" UINT64_FORMAT
					 " physical=%s generation=" UINT64_FORMAT
					 " cost=%s",
					 logical, meta->logical_generation,
					 physical, meta->physical_generation, cost), es);
		ExplainPropertyText("Graph Confidence",
			meta->uncertainty_unknown ? "unknown" : "ready", es);
		ExplainPropertyText("Graph Cost Profile",
			meta->calibrated ?
			psprintf("calibrated v%u generation " UINT64_FORMAT,
					 meta->cost_profile_version, meta->cost_generation) :
			psprintf("default v%u", meta->cost_profile_version), es);
		ExplainPropertyInteger("Graph Covered Relids", NULL,
			meta->covered_relids, es);
		ExplainPropertyInteger("Graph Consumed Quals", NULL,
			meta->consumed_quals, es);
		ExplainPropertyInteger("Graph Required Outer", NULL,
			meta->required_outer, es);
	}
	if (es->analyze && state->fixed)
	{
		GgcFixedInstrumentation instrumentation;

		GgcFixedGetInstrumentation(state->fixed, &instrumentation);
		ExplainPropertyInteger("Graph Startup Revalidations", NULL,
			instrumentation.startup_revalidations, es);
		if (GgcFixedGetStartupMetadataReuses(state->fixed) > 0)
			ExplainPropertyInteger("Graph Startup Metadata Reuses", NULL,
				GgcFixedGetStartupMetadataReuses(state->fixed), es);
		ExplainPropertyInteger("Graph Startup Fallbacks", NULL,
			instrumentation.startup_fallbacks, es);
		ExplainPropertyInteger("Graph Rescans", NULL,
			instrumentation.rescan_count, es);
		ExplainPropertyInteger("Graph Rescan Revalidations", NULL,
			instrumentation.rescan_revalidations, es);
		ExplainPropertyInteger("Graph Runtime Retries", NULL,
			instrumentation.runtime_retries, es);
		ExplainPropertyInteger("Anchor Batches", NULL,
			instrumentation.anchor_batches, es);
		ExplainPropertyInteger("Anchor Bindings", NULL,
			instrumentation.anchor_bindings, es);
		ExplainPropertyInteger("Expanded Edges", NULL,
			instrumentation.expanded_edges, es);
		ExplainPropertyInteger("Output Bindings", NULL,
			instrumentation.output_bindings, es);
		ExplainPropertyInteger("Peak Frontier Bindings", NULL,
			instrumentation.peak_frontier_bindings, es);
		for (uint32 i = 0; i < instrumentation.stage_count; i++)
		{
			const GgcFixedStageInstrumentation *stage = &instrumentation.stages[i];
			char	   *label = psprintf("Stage %u Batch Summary", i + 1);
			char	   *value = psprintf(
				"executions=%llu input=%llu requests=%llu candidates=%llu pages=%llu output=%llu startup_fallback=%s handles=%llu scope_fallbacks=%llu directory=%llu direct_refs=%llu",
				(unsigned long long) stage->executions,
				(unsigned long long) stage->input_bindings,
				(unsigned long long) stage->batch.endpoint_requests,
				(unsigned long long) stage->batch.candidates_received,
				(unsigned long long) stage->batch.heap_page_groups,
				(unsigned long long) stage->output_bindings,
				stage->startup_fallback ? "true" : "false",
				(unsigned long long) stage->handle_inputs,
				(unsigned long long) stage->handle_scope_fallbacks,
				(unsigned long long) stage->batch.provider_directory_lookups,
				(unsigned long long) stage->batch.provider_ref_direct_lookups);

			ExplainPropertyText(label, value, es);
			if (GgcFixedGetIndexLeaseBatches(state->fixed, i))
			{
				char *lease_label = psprintf("Stage %u Index Lease Batches", i + 1);

				ExplainPropertyUInteger(lease_label, NULL,
					GgcFixedGetIndexLeaseBatches(state->fixed, i), es);
			}
			if (GgcFixedGetDirectIndexBatches(state->fixed, i))
			{
				char *direct_label = psprintf("Stage %u Direct Index Batches", i + 1);

				ExplainPropertyUInteger(direct_label, NULL,
					GgcFixedGetDirectIndexBatches(state->fixed, i), es);
			}
			if (stage->batch.batch_setup_us > 0 ||
				stage->batch.provider_getnext_us > 0 ||
				stage->batch.heap_restore_us > 0 ||
				stage->binding_extend_us > 0)
			{
				char *timing_label = psprintf("Stage %u Component Timing", i + 1);
				char *timing_value = psprintf(
					"setup_us=%.3f provider_us=%.3f heap_us=%.3f extend_us=%.3f fetches=%llu restore_peak=%zu",
					stage->batch.batch_setup_us,
					stage->batch.provider_getnext_us,
					stage->batch.heap_restore_us,
					stage->binding_extend_us,
					(unsigned long long) stage->batch.tableam_fetches,
					stage->batch.peak_restore_bytes);

				ExplainPropertyText(timing_label, timing_value, es);
			}
			if (stage->vertex_probes)
			{
				char *vertex_label = psprintf("Stage %u Vertex Visibility", i + 1);
				char *vertex_value = psprintf("probes=%llu rejected=%llu index_only=%llu heap_fetches=%llu scan_starts=%llu",
					(unsigned long long) stage->vertex_probes,
					(unsigned long long) stage->vertex_rejections,
					(unsigned long long) stage->vertex_index_only,
					(unsigned long long) stage->vertex_heap_fetches,
					(unsigned long long) stage->vertex_scan_starts);

				ExplainPropertyText(vertex_label, vertex_value, es);
			}
			if (stage->edge_array_batches || stage->vertex_array_batches)
				ExplainPropertyText(psprintf("Stage %u Array Probes", i + 1),
					psprintf("edge_batches=%llu vertex_batches=%llu",
						(unsigned long long) stage->edge_array_batches,
						(unsigned long long) stage->vertex_array_batches), es);
			if (stage->edge_range_batches || stage->vertex_range_batches)
				ExplainPropertyText(psprintf("Stage %u Range Probes", i + 1),
					psprintf("edge_ranges=%llu vertex_ranges=%llu",
						(unsigned long long) stage->edge_range_batches,
						(unsigned long long) stage->vertex_range_batches), es);
			if (stage->vertex_pointer_sorts)
				ExplainPropertyText(psprintf("Stage %u Vertex Pointer Sort", i + 1),
					psprintf("pointer_sorts=%llu",
						(unsigned long long) stage->vertex_pointer_sorts), es);
			if (stage->endpoint_chunks)
				ExplainPropertyText(psprintf("Stage %u Endpoint Pipeline", i + 1),
					psprintf("chunks=%llu chunk_rows=%llu direct_calls=%llu leases=%llu memory_checks=%llu",
						(unsigned long long) stage->endpoint_chunks,
						(unsigned long long) stage->chunk_rows,
						(unsigned long long) stage->direct_calls,
						(unsigned long long) stage->lease_acquisitions,
						(unsigned long long) stage->memory_checks), es);
			if (state->endpoint_set && (stage->edge_index_only || stage->edge_heap_fetches))
				ExplainPropertyText(psprintf("Stage %u Edge Read", i + 1),
					psprintf("edge_index_only=%llu edge_heap_fetches=%llu",
						(unsigned long long) stage->edge_index_only,
						(unsigned long long) stage->edge_heap_fetches), es);
		}
	}
}
