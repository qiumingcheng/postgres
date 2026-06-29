/* modules/distinct.c — Distinct operator + rules */
#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_distinct_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_UNIQUE, .name = "Unique",
    });
}
