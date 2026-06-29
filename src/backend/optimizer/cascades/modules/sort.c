/* modules/sort.c — Sort operator + rules */
#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_sort_init(void)
{
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_SORT, .name = "Sort",
    });
}
