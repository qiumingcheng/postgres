/* modules/join.c — Join operators (NestLoop, HashJoin, MergeJoin) + rules */
#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_join_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_NESTLOOP, .name = "NestLoop",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_HASHJOIN, .name = "HashJoin",
    });
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_MERGEJOIN, .name = "MergeJoin",
    });
}
