#pragma once
#include <sqlite3.h>
#define SQLITE_VEC_VERSION "v0.1.6"
#define SQLITE_VEC_DATE "2024-11-20"
#define SQLITE_VEC_SOURCE "v0.1.6"
#define SQLITE_VEC_VERSION_MAJOR 0
#define SQLITE_VEC_VERSION_MINOR 1
#define SQLITE_VEC_VERSION_PATCH 6
#define SQLITE_VEC_API
#ifdef __cplusplus
extern "C" {
#endif
int sqlite3_vec_init(sqlite3*, char**, const sqlite3_api_routines*);
#ifdef __cplusplus
}
#endif
