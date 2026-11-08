/*-------------------------------------------------------------------------
 * graph_provider.c
 *
 * Backend-local adjacency-provider registry.  Serializable plans contain a
 * GgcProviderRef, never one of the callback pointers stored here.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "executor/graph_provider.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

typedef struct GgcRegisteredAdjProvider
{
	const GgcAdjProviderV2 *provider;
	const GgcIndexLeaseOps *lease;
	const GgcProviderSetupOps *setup;
	const GgcIndexMetadataOps *metadata;
} GgcRegisteredAdjProvider;

static List *registered_adj_providers = NIL;

static bool
valid_provider(const GgcAdjProviderV2 *provider)
{
	return provider != NULL &&
		provider->struct_size == sizeof(GgcAdjProviderV2) &&
		provider->abi_version == GGC_ADJ_ABI_VERSION &&
		provider->identity.struct_size == sizeof(GgcProviderRef) &&
		provider->identity.abi_version == GGC_ADJ_ABI_VERSION &&
		NameStr(provider->identity.name)[0] != '\0' &&
		memchr(NameStr(provider->identity.name), '\0', NAMEDATALEN) != NULL &&
		provider->validate != NULL && provider->begin_batch != NULL &&
		provider->getnext_batch != NULL && provider->rescan != NULL &&
		provider->end != NULL;
}

const GgcAdjProviderV2 *
GgcGetIndexAdjProvider(Relation index)
{
	const GgcAdjProviderV2 *provider;

	if (index == NULL || index->rd_indam == NULL ||
		index->rd_indam->amgetgraphprovider == NULL)
		return NULL;
	provider = index->rd_indam->amgetgraphprovider();
	if (!valid_provider(provider))
		elog(ERROR, "index AM returned an invalid graph adjacency provider");
	return provider;
}

void
GgcRegisterAdjProvider(const GgcAdjProviderV2 *provider)
{
	MemoryContext old;
	GgcRegisteredAdjProvider *entry;

	if (!valid_provider(provider))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph adjacency provider registration")));
	foreach_ptr(GgcRegisteredAdjProvider, existing, registered_adj_providers)
	{
		if (GgcProviderRefEqual(&existing->provider->identity,
								&provider->identity))
		{
			if (existing->provider == provider)
				return;
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("graph adjacency provider \"%s\" is already registered",
							NameStr(provider->identity.name))));
		}
	}

	old = MemoryContextSwitchTo(TopMemoryContext);
	entry = palloc0(sizeof(*entry));
	entry->provider = provider;
	registered_adj_providers = lappend(registered_adj_providers, entry);
	MemoryContextSwitchTo(old);
}

void
GgcRegisterIndexLease(const GgcAdjProviderV2 *provider,
					  const GgcIndexLeaseOps *ops)
{
	if (!ops || ops->struct_size != sizeof(*ops) ||
		ops->version != GGC_INDEX_LEASE_VERSION || !ops->begin || !ops->finish)
		elog(ERROR, "invalid graph index lease registration");
	GgcRegisterAdjProvider(provider);
	foreach_ptr(GgcRegisteredAdjProvider, entry, registered_adj_providers)
	{
		if (entry->provider != provider)
			continue;
		if (entry->lease && entry->lease != ops)
			elog(ERROR, "graph index lease already registered");
		entry->lease = ops;
		return;
	}
	elog(ERROR, "graph index lease provider is not registered");
}

const GgcIndexLeaseOps *
GgcGetIndexLease(const GgcAdjProviderV2 *provider)
{
	foreach_ptr(GgcRegisteredAdjProvider, entry, registered_adj_providers)
		if (entry->provider == provider)
			return entry->lease;
	return NULL;
}

void
GgcRegisterProviderSetup(const GgcAdjProviderV2 *provider,
						const GgcProviderSetupOps *ops)
{
	if (!ops || ops->struct_size != sizeof(*ops) ||
		ops->version != GGC_PROVIDER_SETUP_VERSION || !ops->validate_open)
		elog(ERROR, "invalid graph provider setup registration");
	GgcRegisterAdjProvider(provider);
	foreach_ptr(GgcRegisteredAdjProvider, entry, registered_adj_providers)
	{
		if (entry->provider != provider)
			continue;
		if (entry->setup && entry->setup != ops)
			elog(ERROR, "graph provider setup already registered");
		entry->setup = ops;
		return;
	}
	elog(ERROR, "graph setup provider is not registered");
}

const GgcProviderSetupOps *
GgcGetProviderSetup(const GgcAdjProviderV2 *provider)
{
	foreach_ptr(GgcRegisteredAdjProvider, entry, registered_adj_providers)
		if (entry->provider == provider)
			return entry->setup;
	return NULL;
}

void
GgcRegisterIndexMetadata(const GgcAdjProviderV2 *provider,
						const GgcIndexMetadataOps *ops)
{
	if (!ops || ops->struct_size != sizeof(*ops) ||
		ops->version != GGC_INDEX_METADATA_VERSION || !ops->build_generation)
		elog(ERROR, "invalid graph index metadata registration");
	GgcRegisterAdjProvider(provider);
	foreach_ptr(GgcRegisteredAdjProvider, entry, registered_adj_providers)
	{
		if (entry->provider != provider)
			continue;
		if (entry->metadata && entry->metadata != ops)
			elog(ERROR, "graph index metadata already registered");
		entry->metadata = ops;
		return;
	}
	elog(ERROR, "graph index metadata provider is not registered");
}

bool
GgcGetIndexBuildGeneration(Relation index, uint64 *generation)
{
	const GgcAdjProviderV2 *provider = GgcGetIndexAdjProvider(index);

	*generation = 0;
	if (!provider)
		return false;
	if (!(provider->capabilities & GGC_CAP_BUILD_GENERATION))
		return true;
	foreach_ptr(GgcRegisteredAdjProvider, entry, registered_adj_providers)
	{
		if (entry->provider == provider && entry->metadata)
		{
			*generation = entry->metadata->build_generation(index);
			return true;
		}
	}
	return false;
}

void
GgcUnregisterAdjProvider(const GgcAdjProviderV2 *provider)
{
	ListCell   *cell;

	foreach(cell, registered_adj_providers)
	{
		GgcRegisteredAdjProvider *entry = lfirst(cell);

		if (entry->provider == provider)
		{
			registered_adj_providers = list_delete_cell(registered_adj_providers,
				cell);
			pfree(entry);
			return;
		}
	}
}

const GgcAdjProviderV2 *
GgcResolveAdjProvider(Relation index, const GgcProviderRef *reference,
					  GgcRejectReason *reason)
{
	const GgcAdjProviderV2 *provider;

	if (reason)
		*reason = GGC_ACCEPT;
	if (reference == NULL || reference->struct_size != sizeof(GgcProviderRef) ||
		reference->abi_version != GGC_ADJ_ABI_VERSION ||
		NameStr(reference->name)[0] == '\0' ||
		memchr(NameStr(reference->name), '\0', NAMEDATALEN) == NULL)
	{
		if (reason)
			*reason = GGC_REJECT_ABI;
		return NULL;
	}
	/* An extension registration must never shadow the index AM's identity. */
	provider = GgcGetIndexAdjProvider(index);
	if (provider && GgcProviderRefEqual(reference, &provider->identity))
		return provider;
	foreach_ptr(GgcRegisteredAdjProvider, entry, registered_adj_providers)
	{
		if (GgcProviderRefEqual(reference, &entry->provider->identity))
			return entry->provider;
	}
	if (reason)
		*reason = GGC_REJECT_CAPABILITY;
	return NULL;
}
