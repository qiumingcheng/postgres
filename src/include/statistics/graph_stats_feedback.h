/*
 * Bounded, query-local graph feedback. Publication is an offline task.
 * A buffer belongs to its creator's effective role/security context and
 * row_security setting. Cross-scope additions are discarded; summaries are
 * empty. Do not retain a buffer across queries or publish it without a full
 * semantic/security key and independent policy/dependency validation.
 */
#ifndef GRAPH_STATS_FEEDBACK_H
#define GRAPH_STATS_FEEDBACK_H

#include "postgres.h"

typedef struct GgcGraphFeedbackRecord
{
	uint64		semantic_fingerprint;
	uint32		stage_count;
	bool		complete;
	double		total_time_us;
	double		actual_output_rows;
} GgcGraphFeedbackRecord;

typedef struct GgcGraphFeedbackSummary
{
	uint32		accepted;
	uint32		dropped;
	uint32		complete_records;
	uint32		incomplete_records;
	uint32		cardinality_records;
	uint32		timing_records;
} GgcGraphFeedbackSummary;

typedef struct GgcGraphFeedbackBuffer GgcGraphFeedbackBuffer;

extern GgcGraphFeedbackBuffer *GgcGraphFeedbackCreate(MemoryContext context,
												   uint32 capacity,
												   uint32 max_stages);
extern bool GgcGraphFeedbackAdd(GgcGraphFeedbackBuffer *buffer,
								 const GgcGraphFeedbackRecord *record);
extern void GgcGraphFeedbackSummarize(const GgcGraphFeedbackBuffer *buffer,
									  GgcGraphFeedbackSummary *summary);

#endif							/* GRAPH_STATS_FEEDBACK_H */
