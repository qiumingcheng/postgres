/* modules/agg.c — Aggregation operators (HashAgg, GroupAgg) + rules */
#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_agg_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_HASHAGG, .name = "HashAgg",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_GROUPAGG, .name = "GroupAgg",
    });
}
