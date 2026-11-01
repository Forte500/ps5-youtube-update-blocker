#ifndef NOTIFY_H
#define NOTIFY_H

#include <stddef.h>

/*
 * Build the JSON payload for a PS5 toast notification.
 *
 * Returns the payload length (NUL excluded) on success, 0 when the
 * buffer is too small or an argument is invalid.
 */
size_t notify_build_payload(char *buf, size_t buflen,
                            const char *body, const char *sub);

/* Send the toast. Returns 0 when the system accepted it. */
int notify_send(const char *body, const char *sub);

#endif /* NOTIFY_H */
