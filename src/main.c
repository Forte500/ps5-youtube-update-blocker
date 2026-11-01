/*
 * youtube-update-blocker
 *
 * Blocks YouTube updates on a jailbroken PS5 by rewriting two rows of
 * /system_data/priv/mms/appinfo.db:
 *
 *   CONTENT_VERSION  -> 99.999.999
 *   VERSION_FILE_URI -> http://localhost   (space padded)
 *
 * The database layout is never changed: only value bytes of the same
 * length are overwritten, so the file stays a valid SQLite database.
 *
 * Run it from a regular ELF loader (elfldr / etaHEN / kstuff). It needs
 * root rights to write the file. Reboot the console afterwards.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "notify.h"
#include "sha256.h"
#include "sqlite_patch.h"

#ifndef DEFAULT_DB
#define DEFAULT_DB  "/system_data/priv/mms/appinfo.db"
#endif
#ifndef BACKUP_PATH
#define BACKUP_PATH "/data/appinfo.db.orig"
#endif
#ifndef LOG_PATH
#define LOG_PATH    "/data/appinfo-block.log"
#endif

static void log_msg(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    printf("%s\n", line);
    fflush(stdout);

    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        size_t len = strlen(line);

        if (write(fd, line, len) < 0 || write(fd, "\n", 1) < 0) {
            /* logging is best effort */
        }
        close(fd);
    }
}

static void toast(int enabled, const char *body, const char *sub)
{
    if (!enabled)
        return;
    if (notify_send(body, sub) != 0)
        log_msg("warning: notification not delivered");
}

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int read_whole_file(const char *path, uint8_t **out, size_t *out_len)
{
    struct stat st;
    uint8_t *buf;
    size_t done = 0;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (done < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + done, (size_t)st.st_size - done);

        if (r <= 0) {
            free(buf);
            close(fd);
            return -1;
        }
        done += (size_t)r;
    }
    close(fd);
    *out = buf;
    *out_len = done;
    return 0;
}

static int write_whole_file(const char *path, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    int fd;

    fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);

        if (w <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    return close(fd) == 0 ? 0 : -1;
}

static int write_backup(const char *path, const uint8_t *buf, size_t len)
{
    char tmp[256];
    int rc;

    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
        return -1;
    unlink(tmp);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);

        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        done += (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);

    rc = rename(tmp, path);
    if (rc != 0)
        unlink(tmp);
    return rc;
}

static void hash_hex(const uint8_t *buf, size_t len, char out[65])
{
    uint8_t digest[32];

    sha256(buf, len, digest);
    sha256_hex(digest, out);
}

static void usage(const char *argv0)
{
    printf("usage: %s [--dry-run] [--no-notify] [--path <appinfo.db>]\n",
           argv0);
    printf("  default path: %s\n", DEFAULT_DB);
    printf("  backup:       %s\n", BACKUP_PATH);
    printf("  log:          %s\n", LOG_PATH);
}

int main(int argc, char **argv)
{
    const char *db_path = DEFAULT_DB;
    uint8_t *orig = NULL;
    size_t len = 0;
    sp_analysis analysis;
    sp_status st;
    char hash_before[65];
    char hash_after[65];
    int dry_run = 0;
    int notify_enabled = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--no-notify") == 0) {
            notify_enabled = 0;
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    log_msg("youtube-update-blocker: %s%s", db_path, dry_run ? " (dry run)" : "");

    if (read_whole_file(db_path, &orig, &len) != 0) {
        log_msg("ERROR: cannot read %s: %s", db_path, strerror(errno));
        toast(notify_enabled, "YouTube block not applied",
              "Cannot read appinfo.db");
        return 1;
    }
    log_msg("size: %zu bytes", len);
    hash_hex(orig, len, hash_before);
    log_msg("sha256 before: %s", hash_before);

    {
        char side[512];
        int sidecars = 0;

        snprintf(side, sizeof(side), "%s-journal", db_path);
        if (file_exists(side))
            sidecars = 1;
        snprintf(side, sizeof(side), "%s-wal", db_path);
        if (file_exists(side))
            sidecars = 1;
        if (sidecars) {
            log_msg("ERROR: a journal/WAL file is present next to the "
                    "database; refusing to touch it");
            toast(notify_enabled, "YouTube block not applied",
                  "Journal/WAL file next to the database");
            free(orig);
            return 3;
        }
    }

    st = sp_analyze(orig, len, &analysis);
    if (st != SP_OK) {
        log_msg("ERROR: %s (%d)", sp_strstatus(st), (int)st);
        toast(notify_enabled, "YouTube block not applied",
              sp_strstatus(st));
        free(orig);
        return 1;
    }
    for (int i = 0; i < analysis.count; i++) {
        const sp_match *m = &analysis.targets[i];

        log_msg("YouTube variant %d/%d: %s", i + 1, analysis.count,
                m->title_id);
        log_msg("  CONTENT_VERSION offset %zu (%zu bytes)",
                m->cv_off, m->cv_len);
        log_msg("  VERSION_FILE_URI offset %zu (%zu bytes)",
                m->uri_off, m->uri_len);
    }

    if (sp_is_patched(orig, &analysis)) {
        log_msg("already blocked, nothing to do");
        toast(notify_enabled, "YouTube already blocked",
              "Nothing to do");
        free(orig);
        return 0;
    }

    if (dry_run) {
        log_msg("dry run: the patch would be applied");
        toast(notify_enabled, "Analysis complete (dry run)",
              "No changes applied");
        free(orig);
        return 0;
    }

    if (write_backup(BACKUP_PATH, orig, len) != 0) {
        log_msg("ERROR: cannot write backup %s: %s (database untouched)",
                BACKUP_PATH, strerror(errno));
        toast(notify_enabled, "YouTube block not applied",
              "Backup failed, database untouched");
        free(orig);
        return 4;
    }
    log_msg("backup written to %s", BACKUP_PATH);

    st = sp_patch(orig, len, &analysis);
    if (st != SP_OK && st != SP_ALREADY_PATCHED) {
        log_msg("ERROR: patch failed: %s (%d)", sp_strstatus(st), (int)st);
        toast(notify_enabled, "YouTube block not applied",
              sp_strstatus(st));
        free(orig);
        return 1;
    }

    hash_hex(orig, len, hash_after);
    log_msg("sha256 after patch: %s", hash_after);

    if (write_whole_file(db_path, orig, len) != 0) {
        log_msg("ERROR: cannot write %s: %s", db_path, strerror(errno));
        toast(notify_enabled, "YouTube block not applied",
              "Writing the database failed");
        free(orig);
        return 1;
    }

    /* Read the file back and make sure what is on disk is what we wrote. */
    {
        uint8_t *check = NULL;
        size_t check_len = 0;

        if (read_whole_file(db_path, &check, &check_len) != 0) {
            log_msg("ERROR: cannot re-read %s after writing", db_path);
            toast(notify_enabled, "YouTube block not applied",
                  "On-disk verification failed");
            free(orig);
            return 1;
        }
        char hash_disk[65];
        hash_hex(check, check_len, hash_disk);
        free(check);
        if (check_len != len || strcmp(hash_disk, hash_after) != 0) {
            log_msg("ERROR: on-disk hash mismatch: %s", hash_disk);
            toast(notify_enabled, "YouTube block not applied",
                  "On-disk hash mismatch");
            free(orig);
            return 1;
        }
        log_msg("on-disk sha256 verified: %s", hash_disk);
    }

    free(orig);
    log_msg("done: YouTube updates blocked. Reboot the console now.");
    toast(notify_enabled, "YouTube updates blocked",
          "Reboot the console to apply the change");
    return 0;
}
