#ifndef SQLITE_PATCH_H
#define SQLITE_PATCH_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal, read-only SQLite table b-tree walker used to locate two values
 * inside a single table and overwrite them in place without changing a
 * single byte of the database structure.
 *
 * Supported schema (the one used by /system_data/priv/mms/appinfo.db):
 *   tbl_appinfo(titleId TEXT, metaDataId TEXT, key, val,
 *               PRIMARY KEY(titleId, metaDataId, key))
 *
 * YouTube is recognised by the CONTENT_ID token and, as a fallback, by
 * the known titleIds of the US/EU/JP variants, so the same payload works
 * on any region and blocks every variant present in the database.
 */

#define SP_MAX_TARGETS 8

typedef enum {
    SP_OK                = 0,
    SP_ALREADY_PATCHED   = 1,
    SP_ERR_ARG           = -1,
    SP_ERR_SHORT         = -2,
    SP_ERR_HEADER        = -3,
    SP_ERR_PAGESIZE      = -4,
    SP_ERR_SCHEMA        = -5,
    SP_ERR_BTREE         = -6,
    SP_ERR_NO_YOUTUBE    = -7,
    SP_ERR_MULTIPLE_YOUTUBE = -8,
    SP_ERR_MISSING_FIELD = -9,
    SP_ERR_DUPLICATE_FIELD = -10,
    SP_ERR_UNEXPECTED_VALUE = -11,
    SP_ERR_OVERFLOW      = -12,
    SP_ERR_TOO_MANY_TARGETS = -13
} sp_status;

typedef struct {
    char   title_id[16]; /* NUL terminated, e.g. "PPSA01651" */
    size_t cv_off;       /* absolute offset of CONTENT_VERSION value bytes */
    size_t cv_len;
    size_t uri_off;      /* absolute offset of VERSION_FILE_URI value bytes */
    size_t uri_len;
} sp_match;

typedef struct {
    int      count;
    sp_match targets[SP_MAX_TARGETS];
} sp_analysis;

const char *sp_strstatus(sp_status st);

/* Analyse only: locate every YouTube variant and the rows to patch. */
sp_status sp_analyze(const uint8_t *db, size_t len, sp_analysis *out);

/*
 * Patch every target in place (same length, no structural change).
 * Returns SP_OK when bytes were changed, SP_ALREADY_PATCHED when every
 * target already carries the blocked values.
 */
sp_status sp_patch(uint8_t *db, size_t len, sp_analysis *out);

/* True when every target already holds the blocked form. */
int sp_is_patched(const uint8_t *db, const sp_analysis *a);

#endif /* SQLITE_PATCH_H */
