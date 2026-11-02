#ifndef CILO_DSL_H
#define CILO_DSL_H

#include "cilo.h"
#include "map.h"

#define new(...) CILO_NEW(__VA_ARGS__)
#define new_array(...) CILO_NEW_ARRAY(__VA_ARGS__)
#define delete(...) CILO_DELETE(__VA_ARGS__)
#define vec(...) CILO_VEC(__VA_ARGS__)
#define push(...) CILO_PUSH(__VA_ARGS__)
#define reserve(...) CILO_RESERVE(__VA_ARGS__)
#define pool(...) CILO_POOL(__VA_ARGS__)
#define take(...) CILO_TAKE(__VA_ARGS__)
#define give(...) CILO_GIVE(__VA_ARGS__)
#define pool_drop(...) CILO_POOL_DROP(__VA_ARGS__)
#define rewinding(...) CILO_REWINDING(__VA_ARGS__)
#define with_arena(...) CILO_WITH_ARENA(__VA_ARGS__)
#define scratch(...) CILO_SCRATCH(__VA_ARGS__)
#define arena_on(...) CILO_ARENA_ON(__VA_ARGS__)
#define arena_fixed(...) CILO_ARENA_FIXED(__VA_ARGS__)
#define astrdup(...) CILO_STRDUP(__VA_ARGS__)
#define aprintf(...) CILO_PRINTF(__VA_ARGS__)
#define map(...) CILO_MAP(__VA_ARGS__)
#define map_get(...) CILO_MAP_GET(__VA_ARGS__)
#define map_get_n(...) CILO_MAP_GET_N(__VA_ARGS__)
#define map_has(...) CILO_MAP_HAS(__VA_ARGS__)
#define map_put(...) CILO_MAP_PUT(__VA_ARGS__)
#define map_del(...) CILO_MAP_DEL(__VA_ARGS__)
#define map_each(...) CILO_MAP_EACH(__VA_ARGS__)
#define map_clear(...) CILO_MAP_CLEAR(__VA_ARGS__)
#define map_drop(...) CILO_MAP_DROP(__VA_ARGS__)

#endif
