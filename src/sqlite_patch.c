/*
 * Locate two rows inside tbl_appinfo and overwrite their values in place.
 *
 * The patch never changes a record, cell or page length: it only rewrites
 * bytes inside the value fields, so the SQLite file stays structurally
 * identical. That is why we can avoid carrying a full SQLite engine.
 */

#include "sqlite_patch.h"

#include <string.h>

#define SP_MAX_DEPTH  32
#define SP_MAX_FIELDS 8
#define SP_STOP       ((sp_status)1000)

typedef struct {
    const uint8_t *db;
    size_t         len;
    uint32_t       page_size;
    uint32_t       usable;
    uint32_t       page_count;
} sp_db;

typedef struct {
    uint32_t type;
    uint32_t body_off; /* relative to the record start */
    uint32_t len;
} sp_field;

typedef struct {
    uint32_t page_no;
    uint32_t payload_off; /* relative to the page start */
    uint32_t payload_len;
} sp_cell;

typedef sp_status (*sp_cell_cb)(void *ctx, const sp_db *d,
                                const sp_cell *cell,
                                const uint8_t *rec, uint32_t rec_len);

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int get_varint(const uint8_t *buf, size_t end, size_t *pos,
                      uint64_t *out)
{
    uint64_t v = 0;
    size_t i = *pos;

    for (int k = 0; k < 9; k++) {
        uint8_t c;

        if (i >= end)
            return -1;
        c = buf[i++];
        if (k == 8) {
            v = (v << 8) | (uint64_t)c;
            *out = v;
            *pos = i;
            return 0;
        }
        v = (v << 7) | (uint64_t)(c & 0x7f);
        if (!(c & 0x80)) {
            *out = v;
            *pos = i;
            return 0;
        }
    }
    return -1;
}

static int record_parse(const uint8_t *rec, uint32_t rec_len,
                        sp_field *fields, int max_fields)
{
    size_t pos = 0;
    size_t hdr_end;
    size_t body;
    size_t tp;
    uint64_t hdr;
    int n = 0;

    if (get_varint(rec, rec_len, &pos, &hdr))
        return -1;
    if (hdr < pos || hdr > rec_len)
        return -1;

    hdr_end = (size_t)hdr;
    body = hdr_end;
    tp = pos;

    while (tp < hdr_end) {
        uint64_t t;
        uint32_t flen;

        if (get_varint(rec, hdr_end, &tp, &t))
            return -1;

        if (t == 0 || t == 8 || t == 9)
            flen = 0;
        else if (t >= 1 && t <= 4)
            flen = (uint32_t)t;
        else if (t == 5)
            flen = 6;
        else if (t == 6 || t == 7)
            flen = 8;
        else if (t >= 12)
            flen = (uint32_t)((t - 12) / 2);
        else
            return -1; /* 10 and 11 are reserved */

        if (n >= max_fields)
            return -1;
        fields[n].type = (uint32_t)t;
        fields[n].body_off = (uint32_t)body;
        fields[n].len = flen;
        n++;
        body += flen;
        if (body > rec_len)
            return -1;
    }
    return n;
}

static int field_is_text(const sp_field *f)
{
    return f->type >= 13 && (f->type & 1);
}

static int field_is_int(const sp_field *f)
{
    return (f->type >= 1 && f->type <= 6) || f->type == 8 || f->type == 9;
}

static int field_int_value(const uint8_t *rec, const sp_field *f,
                           uint32_t *out)
{
    const uint8_t *b = rec + f->body_off;
    uint64_t v = 0;

    switch (f->type) {
    case 1:
        v = b[0];
        break;
    case 2:
        v = ((uint64_t)b[0] << 8) | b[1];
        break;
    case 3:
        v = ((uint64_t)b[0] << 16) | ((uint64_t)b[1] << 8) | b[2];
        break;
    case 4:
        v = ((uint64_t)b[0] << 24) | ((uint64_t)b[1] << 16) |
            ((uint64_t)b[2] << 8) | b[3];
        break;
    case 5:
        for (int i = 0; i < 6; i++)
            v = (v << 8) | b[i];
        break;
    case 6:
        for (int i = 0; i < 8; i++)
            v = (v << 8) | b[i];
        break;
    case 8:
        v = 0;
        break;
    case 9:
        v = 1;
        break;
    default:
        return 0;
    }
    if (v > 0xffffffffull)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static int contains(const uint8_t *hay, uint32_t hay_len,
                    const char *needle, size_t needle_len)
{
    if (needle_len > hay_len)
        return 0;
    for (uint32_t i = 0; i + needle_len <= hay_len; i++)
        if (memcmp(hay + i, needle, needle_len) == 0)
            return 1;
    return 0;
}

/*
 * Number of payload bytes stored on the b-tree page itself. When it is
 * smaller than the full payload the rest lives on overflow pages, which
 * this walker deliberately does not follow.
 */
static uint32_t local_payload_size(uint32_t payload, uint32_t usable,
                                   int interior)
{
    uint32_t max_local;
    uint32_t min_local;
    uint32_t k;

    max_local = interior ? ((usable - 12) * 64 / 255) - 23 : usable - 35;
    if (payload <= max_local)
        return payload;

    min_local = ((usable - 12) * 32 / 255) - 23;
    if (min_local > max_local)
        min_local = max_local;
    k = min_local + (payload - min_local) % (usable - 4);
    return (k <= max_local) ? k : min_local;
}

static sp_status walk_tree(const sp_db *d, uint32_t page_no, int depth,
                           sp_cell_cb cb, void *ctx, int *overflow)
{
    size_t page_off;
    const uint8_t *p;
    size_t hdr;
    uint8_t type;
    uint32_t ncells;
    uint32_t ptr_base;
    sp_status st;

    if (depth > SP_MAX_DEPTH)
        return SP_ERR_BTREE;
    if (page_no < 1 || page_no > d->page_count)
        return SP_ERR_BTREE;

    page_off = (size_t)(page_no - 1) * d->page_size;
    if (page_off + d->page_size > d->len)
        return SP_ERR_BTREE;
    p = d->db + page_off;

    hdr = (page_no == 1) ? 100 : 0;
    if (hdr + 8 > d->usable)
        return SP_ERR_BTREE;

    type = p[hdr];
    ncells = be16(p + hdr + 3);
    ptr_base = (uint32_t)hdr + (type == 5 ? 12u : 8u);
    if ((size_t)ptr_base + (size_t)ncells * 2 > d->usable)
        return SP_ERR_BTREE;

    if (type == 5) {
        uint32_t right = be32(p + hdr + 8);

        for (uint32_t i = 0; i < ncells; i++) {
            uint32_t cp = be16(p + ptr_base + (size_t)i * 2);

            if ((size_t)cp + 4 > d->usable)
                return SP_ERR_BTREE;
            st = walk_tree(d, be32(p + cp), depth + 1, cb, ctx, overflow);
            if (st != SP_OK)
                return st;
        }
        return walk_tree(d, right, depth + 1, cb, ctx, overflow);
    }

    if (type != 13)
        return SP_ERR_BTREE;

    for (uint32_t i = 0; i < ncells; i++) {
        uint32_t cp = be16(p + ptr_base + (size_t)i * 2);
        size_t pos;
        uint64_t payload;
        uint64_t rowid;
        uint32_t local;
        sp_cell cell;

        if (cp >= d->usable)
            return SP_ERR_BTREE;
        pos = cp;
        if (get_varint(p, d->usable, &pos, &payload))
            return SP_ERR_BTREE;
        if (get_varint(p, d->usable, &pos, &rowid))
            return SP_ERR_BTREE;
        (void)rowid;
        if (payload > 0xffffffffull)
            return SP_ERR_BTREE;

        local = local_payload_size((uint32_t)payload, d->usable, 0);
        if (local < (uint32_t)payload) {
            if (overflow)
                *overflow = 1;
            continue;
        }
        if (pos + (size_t)payload > d->usable)
            return SP_ERR_BTREE;

        cell.page_no = page_no;
        cell.payload_off = (uint32_t)pos;
        cell.payload_len = (uint32_t)payload;

        st = cb(ctx, d, &cell, p + pos, (uint32_t)payload);
        if (st != SP_OK)
            return st;
    }
    return SP_OK;
}

/* ---------------------------------------------------------------- */
/* sqlite_master lookup                                              */

typedef struct {
    const char *name;
    uint32_t    root;
    int         found;
} schema_ctx;

static sp_status cb_schema(void *vctx, const sp_db *d, const sp_cell *cell,
                           const uint8_t *rec, uint32_t rec_len)
{
    schema_ctx *ctx = vctx;
    sp_field f[SP_MAX_FIELDS];
    int n;

    (void)d;
    (void)cell;

    n = record_parse(rec, rec_len, f, SP_MAX_FIELDS);
    if (n < 5)
        return SP_OK;
    if (!field_is_text(&f[0]) || !field_is_text(&f[1]))
        return SP_OK;
    if (f[0].len != 5 || memcmp(rec + f[0].body_off, "table", 5) != 0)
        return SP_OK;
    if (f[1].len != strlen(ctx->name))
        return SP_OK;
    if (memcmp(rec + f[1].body_off, ctx->name, f[1].len) != 0)
        return SP_OK;
    if (!field_is_int(&f[3]) || !field_int_value(rec, &f[3], &ctx->root))
        return SP_OK;

    ctx->found = 1;
    return SP_STOP;
}

/* ---------------------------------------------------------------- */
/* pass 1: find the YouTube titleId                                  */

static const char *const KNOWN_YOUTUBE_IDS[] = {
    "PPSA01650", /* US */
    "PPSA01651", /* EU */
    "PPSA01652"  /* JP */
};

static int is_known_youtube_id(const uint8_t *id, size_t len)
{
    for (size_t i = 0;
         i < sizeof(KNOWN_YOUTUBE_IDS) / sizeof(KNOWN_YOUTUBE_IDS[0]); i++)
        if (len == 9 && memcmp(id, KNOWN_YOUTUBE_IDS[i], 9) == 0)
            return 1;
    return 0;
}

typedef struct {
    char   titles[SP_MAX_TARGETS][16];
    size_t lens[SP_MAX_TARGETS];
    int    count;
    int    too_many;
} find_ctx;

static int find_title_slot(const find_ctx *ctx, const uint8_t *id, size_t len)
{
    for (int i = 0; i < ctx->count; i++)
        if (ctx->lens[i] == len && memcmp(ctx->titles[i], id, len) == 0)
            return i;
    return -1;
}

static sp_status find_add(find_ctx *ctx, const uint8_t *id, size_t len)
{
    if (find_title_slot(ctx, id, len) >= 0)
        return SP_OK;
    if (ctx->count >= SP_MAX_TARGETS) {
        ctx->too_many = 1;
        return SP_STOP;
    }
    memcpy(ctx->titles[ctx->count], id, len);
    ctx->titles[ctx->count][len] = '\0';
    ctx->lens[ctx->count] = len;
    ctx->count++;
    return SP_OK;
}

static sp_status cb_find_youtube(void *vctx, const sp_db *d,
                                 const sp_cell *cell,
                                 const uint8_t *rec, uint32_t rec_len)
{
    find_ctx *ctx = vctx;
    sp_field f[SP_MAX_FIELDS];
    int candidate;
    int n;

    (void)d;
    (void)cell;

    n = record_parse(rec, rec_len, f, SP_MAX_FIELDS);
    if (n < 4)
        return SP_OK;
    if (!field_is_text(&f[0]) || f[0].len == 0 || f[0].len > 15)
        return SP_OK;

    /* The region variants we know by heart... */
    candidate = is_known_youtube_id(rec + f[0].body_off, f[0].len);

    /* ...plus any title advertising itself through CONTENT_ID. */
    if (!candidate && field_is_text(&f[2]) && f[2].len == 10 &&
        memcmp(rec + f[2].body_off, "CONTENT_ID", 10) == 0 &&
        field_is_text(&f[3]) &&
        contains(rec + f[3].body_off, f[3].len, "YOUTUBE", 7))
        candidate = 1;

    if (!candidate)
        return SP_OK;
    return find_add(ctx, rec + f[0].body_off, f[0].len);
}

/* ---------------------------------------------------------------- */
/* pass 2: record the two value locations                            */

typedef struct {
    sp_analysis *out;
    int has_version[SP_MAX_TARGETS];
    int has_uri[SP_MAX_TARGETS];
} fields_ctx;

static int target_index(const sp_analysis *a, const uint8_t *id, size_t len)
{
    for (int i = 0; i < a->count; i++)
        if (strlen(a->targets[i].title_id) == len &&
            memcmp(a->targets[i].title_id, id, len) == 0)
            return i;
    return -1;
}

static sp_status cb_fields(void *vctx, const sp_db *d, const sp_cell *cell,
                           const uint8_t *rec, uint32_t rec_len)
{
    fields_ctx *ctx = vctx;
    sp_field f[SP_MAX_FIELDS];
    size_t abs_off;
    int idx;
    int n;

    n = record_parse(rec, rec_len, f, SP_MAX_FIELDS);
    if (n < 4)
        return SP_OK;
    if (!field_is_text(&f[0]) || !field_is_text(&f[2]))
        return SP_OK;

    idx = target_index(ctx->out, rec + f[0].body_off, f[0].len);
    if (idx < 0)
        return SP_OK;

    abs_off = (size_t)(cell->page_no - 1) * d->page_size +
              cell->payload_off + f[3].body_off;

    if (f[2].len == 15 &&
        memcmp(rec + f[2].body_off, "CONTENT_VERSION", 15) == 0) {
        if (!field_is_text(&f[3]))
            return SP_ERR_UNEXPECTED_VALUE;
        if (ctx->has_version[idx])
            return SP_ERR_DUPLICATE_FIELD;
        ctx->has_version[idx] = 1;
        ctx->out->targets[idx].cv_off = abs_off;
        ctx->out->targets[idx].cv_len = f[3].len;
        return SP_OK;
    }

    if (f[2].len == 16 &&
        memcmp(rec + f[2].body_off, "VERSION_FILE_URI", 16) == 0) {
        if (!field_is_text(&f[3]))
            return SP_ERR_UNEXPECTED_VALUE;
        if (ctx->has_uri[idx])
            return SP_ERR_DUPLICATE_FIELD;
        ctx->has_uri[idx] = 1;
        ctx->out->targets[idx].uri_off = abs_off;
        ctx->out->targets[idx].uri_len = f[3].len;
        return SP_OK;
    }

    return SP_OK;
}

/* ---------------------------------------------------------------- */

static int version_looks_valid(const uint8_t *v, size_t len)
{
    if (len != 10)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (i == 2 || i == 6) {
            if (v[i] != '.')
                return 0;
        } else if (v[i] < '0' || v[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static int uri_looks_valid(const uint8_t *v, size_t len)
{
    if (len < 16)
        return 0;
    return memcmp(v, "https://", 8) == 0 || memcmp(v, "http://", 7) == 0;
}

const char *sp_strstatus(sp_status st)
{
    switch (st) {
    case SP_OK:                 return "ok";
    case SP_ALREADY_PATCHED:    return "already patched";
    case SP_ERR_ARG:            return "invalid argument";
    case SP_ERR_SHORT:          return "file too short";
    case SP_ERR_HEADER:         return "not a SQLite 3 database";
    case SP_ERR_PAGESIZE:       return "unsupported page size";
    case SP_ERR_SCHEMA:         return "tbl_appinfo not found";
    case SP_ERR_BTREE:          return "malformed or truncated b-tree";
    case SP_ERR_NO_YOUTUBE:     return "no YouTube entry found";
    case SP_ERR_MULTIPLE_YOUTUBE:
        return "more than one YouTube entry found";
    case SP_ERR_TOO_MANY_TARGETS:
        return "too many YouTube entries";
    case SP_ERR_MISSING_FIELD:  return "CONTENT_VERSION or VERSION_FILE_URI missing";
    case SP_ERR_DUPLICATE_FIELD:
        return "duplicated CONTENT_VERSION or VERSION_FILE_URI";
    case SP_ERR_UNEXPECTED_VALUE:
        return "value has an unexpected type, length or format";
    case SP_ERR_OVERFLOW:       return "a row overflows its b-tree page";
    default:                    return "unknown status";
    }
}

sp_status sp_analyze(const uint8_t *db, size_t len, sp_analysis *out)
{
    sp_db d;
    sp_status st;
    uint32_t declared;
    size_t pages_by_len;
    int dirty = 0;
    schema_ctx sc;
    find_ctx fc;
    fields_ctx pc;

    if (!db || !out)
        return SP_ERR_ARG;
    memset(out, 0, sizeof(*out));

    if (len < 100)
        return SP_ERR_SHORT;
    if (memcmp(db, "SQLite format 3\0", 16) != 0)
        return SP_ERR_HEADER;

    d.db = db;
    d.len = len;
    d.page_size = be16(db + 16);
    if (d.page_size == 1)
        d.page_size = 65536;
    if (d.page_size < 512 || d.page_size > 65536 ||
        (d.page_size & (d.page_size - 1)) != 0)
        return SP_ERR_PAGESIZE;
    if (db[20] >= d.page_size)
        return SP_ERR_PAGESIZE;
    d.usable = d.page_size - db[20];
    if (d.usable < 480)
        return SP_ERR_PAGESIZE;

    declared = be32(db + 28);
    pages_by_len = len / d.page_size;
    if (declared == 0 || (size_t)declared > pages_by_len)
        return SP_ERR_BTREE;
    d.page_count = declared;

    sc.name = "tbl_appinfo";
    sc.root = 0;
    sc.found = 0;
    st = walk_tree(&d, 1, 0, cb_schema, &sc, NULL);
    if (st == SP_STOP)
        st = SP_OK;
    if (st != SP_OK)
        return st;
    if (!sc.found || sc.root < 1 || sc.root > d.page_count)
        return SP_ERR_SCHEMA;

    memset(&fc, 0, sizeof(fc));
    dirty = 0;
    st = walk_tree(&d, sc.root, 0, cb_find_youtube, &fc, &dirty);
    if (st == SP_STOP)
        st = SP_OK;
    if (st != SP_OK)
        return st;
    if (fc.too_many)
        return SP_ERR_TOO_MANY_TARGETS;
    if (fc.count == 0)
        return SP_ERR_NO_YOUTUBE;

    out->count = fc.count;
    for (int i = 0; i < fc.count; i++)
        memcpy(out->targets[i].title_id, fc.titles[i], fc.lens[i] + 1);

    memset(&pc, 0, sizeof(pc));
    pc.out = out;
    dirty = 0;
    st = walk_tree(&d, sc.root, 0, cb_fields, &pc, &dirty);
    if (st != SP_OK)
        return st;
    if (dirty)
        return SP_ERR_OVERFLOW;

    for (int i = 0; i < out->count; i++) {
        sp_match *m = &out->targets[i];

        if (!pc.has_version[i] || !pc.has_uri[i])
            return SP_ERR_MISSING_FIELD;
        if (m->cv_off + m->cv_len > len || m->uri_off + m->uri_len > len)
            return SP_ERR_BTREE;
        if (!version_looks_valid(db + m->cv_off, m->cv_len))
            return SP_ERR_UNEXPECTED_VALUE;
        if (!uri_looks_valid(db + m->uri_off, m->uri_len))
            return SP_ERR_UNEXPECTED_VALUE;
    }

    return SP_OK;
}

static int starts_with(const uint8_t *v, size_t len, const char *prefix,
                       size_t prefix_len)
{
    return len >= prefix_len && memcmp(v, prefix, prefix_len) == 0;
}

int sp_is_patched(const uint8_t *db, const sp_analysis *a)
{
    if (!db || !a || a->count <= 0)
        return 0;
    for (int i = 0; i < a->count; i++) {
        const sp_match *m = &a->targets[i];

        if (m->cv_len != 10 ||
            memcmp(db + m->cv_off, "99.999.999", 10) != 0)
            return 0;
        if (!starts_with(db + m->uri_off, m->uri_len,
                         "http://localhost", 16))
            return 0;
    }
    return 1;
}

sp_status sp_patch(uint8_t *db, size_t len, sp_analysis *out)
{
    static const char new_version[10] = "99.999.999";
    static const char new_uri[16] = "http://localhost";
    sp_analysis before;
    sp_analysis check;
    sp_status st;
    int changed = 0;

    if (!db || !out)
        return SP_ERR_ARG;

    st = sp_analyze(db, len, &before);
    if (st != SP_OK)
        return st;

    for (int i = 0; i < before.count; i++) {
        sp_match *m = &before.targets[i];
        int cv_done = m->cv_len == 10 &&
                      memcmp(db + m->cv_off, new_version, 10) == 0;
        int uri_done = starts_with(db + m->uri_off, m->uri_len, new_uri, 16);

        if (cv_done && uri_done)
            continue;
        if (!cv_done)
            memcpy(db + m->cv_off, new_version, 10);
        if (!uri_done) {
            memcpy(db + m->uri_off, new_uri, 16);
            memset(db + m->uri_off + 16, ' ', m->uri_len - 16);
        }
        changed = 1;
    }
    if (!changed) {
        *out = before;
        return SP_ALREADY_PATCHED;
    }

    /* Re-walk the tree on the patched buffer: the structure must still
     * parse and the values must be the ones we just wrote. */
    st = sp_analyze(db, len, &check);
    if (st != SP_OK)
        return SP_ERR_UNEXPECTED_VALUE;
    if (check.count != before.count)
        return SP_ERR_BTREE;
    for (int i = 0; i < check.count; i++) {
        if (strcmp(check.targets[i].title_id,
                   before.targets[i].title_id) != 0 ||
            check.targets[i].cv_off != before.targets[i].cv_off ||
            check.targets[i].cv_len != before.targets[i].cv_len ||
            check.targets[i].uri_off != before.targets[i].uri_off ||
            check.targets[i].uri_len != before.targets[i].uri_len)
            return SP_ERR_BTREE;
    }
    if (!sp_is_patched(db, &check))
        return SP_ERR_UNEXPECTED_VALUE;

    *out = check;
    return SP_OK;
}
