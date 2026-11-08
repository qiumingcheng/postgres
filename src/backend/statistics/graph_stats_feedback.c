/*-------------------------------------------------------------------------
 * graph_stats_feedback.c
 *
 * Query-local, bounded feedback collection.  A full queue drops new records
 * instead of blocking execution.  Incomplete executions retain timing data
 * but are never counted as complete-cardinality observations.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"
#include "statistics/graph_stats_feedback.h"
#include "utils/memutils.h"
#include "utils/rls.h"

struct GgcGraphFeedbackBuffer
{
	uint32		capacity;
	uint32		max_stages;
	uint32		count;
	uint32		dropped;
	Oid			owner;
	int			security_context;
	bool		row_security_enabled;
	GgcGraphFeedbackRecord *records;
};

/* A query-local buffer must not become a cross-role feedback channel. */
static bool
feedback_scope_matches(const GgcGraphFeedbackBuffer *buffer)
{
	Oid			user;
	int			context;

	GetUserIdAndSecContext(&user, &context);
	return user == buffer->owner && context == buffer->security_context &&
		row_security == buffer->row_security_enabled;
}

GgcGraphFeedbackBuffer *
GgcGraphFeedbackCreate(MemoryContext context, uint32 capacity,
					   uint32 max_stages)
{
	MemoryContext old;
	GgcGraphFeedbackBuffer *buffer;

	if (!context || capacity == 0 || max_stages == 0 ||
		!AllocSizeIsValid((Size) capacity * sizeof(GgcGraphFeedbackRecord)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid bounded graph feedback configuration")));
	old = MemoryContextSwitchTo(context);
	buffer = palloc0(sizeof(*buffer));
	buffer->records = palloc0((Size) capacity * sizeof(*buffer->records));
	MemoryContextSwitchTo(old);
	buffer->capacity = capacity;
	buffer->max_stages = max_stages;
	GetUserIdAndSecContext(&buffer->owner, &buffer->security_context);
	buffer->row_security_enabled = row_security;
	return buffer;
}

bool
GgcGraphFeedbackAdd(GgcGraphFeedbackBuffer *buffer,
					const GgcGraphFeedbackRecord *record)
{
	if (!buffer || !record || record->stage_count == 0 ||
		record->stage_count > buffer->max_stages ||
		!isfinite(record->total_time_us) || record->total_time_us < 0 ||
		!isfinite(record->actual_output_rows) || record->actual_output_rows < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph feedback record")));
	/* Feedback is optional: discard mismatches without failing the query. */
	if (!feedback_scope_matches(buffer))
		return false;
	if (buffer->count == buffer->capacity)
	{
		buffer->dropped++;
		return false;
	}
	buffer->records[buffer->count++] = *record;
	return true;
}

void
GgcGraphFeedbackSummarize(const GgcGraphFeedbackBuffer *buffer,
						  GgcGraphFeedbackSummary *summary)
{
	if (!buffer || !summary)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph feedback summary request")));
	MemSet(summary, 0, sizeof(*summary));
	if (!feedback_scope_matches(buffer))
		return;
	summary->accepted = buffer->count;
	summary->dropped = buffer->dropped;
	summary->timing_records = buffer->count;
	for (uint32 i = 0; i < buffer->count; i++)
	{
		if (buffer->records[i].complete)
		{
			summary->complete_records++;
			summary->cardinality_records++;
		}
		else
			summary->incomplete_records++;
	}
}
