/* modules/limit.c — Limit operator + rules */
#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_limit_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_LIMIT, .name = "Limit",
    });
}
