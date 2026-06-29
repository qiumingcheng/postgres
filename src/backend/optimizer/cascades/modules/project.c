#include "postgres.h"
#include "optimizer/cascades.h"
void pg_module_project_init(void) {
    pg_registry_register_operator(&(PgOperatorVtable){
        .op = PG_CASCADES_PHYSICAL_PROJECT, .name = "Project",
    });
}
