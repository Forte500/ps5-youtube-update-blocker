/*
 * PS5 toast notification (libSceNotification).
 *
 * The JSON template follows the SDK sample in
 * ps5-payload-sdk/samples/notify, with the icon switched to a predefined
 * one so no local image path is required.
 */

#include "notify.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef SP_HOST
extern int sceNotificationSend(int userId, bool isLogged,
                               const char *payload);
#endif

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define NOTIFY_PAYLOAD_MAX 4096

static const char TOAST_TEMPLATE[] =
    "{\n"
    "  \"rawData\": {\n"
    "    \"viewTemplateType\": \"InteractiveToastTemplateB\",\n"
    "    \"channelType\": \"Downloads\",\n"
    "    \"useCaseId\": \"IDC\",\n"
    "    \"toastOverwriteType\": \"No\",\n"
    "    \"isImmediate\": true,\n"
    "    \"priority\": 100,\n"
    "    \"viewData\": {\n"
    "      \"icon\": {\n"
    "        \"type\": \"Predefined\",\n"
    "        \"parameters\": { \"icon\": \"download\" }\n"
    "      },\n"
    "      \"message\": { \"body\": \"%s\" },\n"
    "      \"subMessage\": { \"body\": \"%s\" }\n"
    "    },\n"
    "    \"platformViews\": {\n"
    "      \"previewDisabled\": {\n"
    "        \"viewData\": {\n"
    "          \"icon\": {\n"
    "            \"type\": \"Predefined\",\n"
    "            \"parameters\": { \"icon\": \"download\" }\n"
    "          },\n"
    "          \"message\": { \"body\": \"%s\" }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "  },\n"
    "  \"createdDateTime\": \"%s\",\n"
    "  \"localNotificationId\": \"588193127\"\n"
    "}\n";

static int json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        const char *rep = NULL;
        char unicode[8];

        switch (*p) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\b': rep = "\\b";  break;
        case '\f': rep = "\\f";  break;
        case '\n': rep = "\\n";  break;
        case '\r': rep = "\\r";  break;
        case '\t': rep = "\\t";  break;
        default:
            if (*p < 0x20) {
                snprintf(unicode, sizeof(unicode), "\\u%04x", (unsigned)*p);
                rep = unicode;
            }
            break;
        }

        if (rep) {
            size_t n = strlen(rep);

            if (o + n + 1 > cap)
                return -1;
            memcpy(dst + o, rep, n);
            o += n;
        } else {
            if (o + 2 > cap)
                return -1;
            dst[o++] = (char)*p;
        }
    }
    if (o + 1 > cap)
        return -1;
    dst[o] = '\0';
    return 0;
}

static void iso_timestamp(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm *tm_utc = now != (time_t)-1 ? gmtime(&now) : NULL;

    if (tm_utc != NULL &&
        strftime(out, cap, "%Y-%m-%dT%H:%M:%S.000Z", tm_utc) > 0)
        return;
    snprintf(out, cap, "2025-12-14T03:14:51.473Z");
}

size_t notify_build_payload(char *buf, size_t buflen,
                            const char *body, const char *sub)
{
    char esc_body[512];
    char esc_sub[512];
    char timestamp[32];
    int n;

    if (!buf || buflen == 0 || !body || !sub)
        return 0;
    if (json_escape(esc_body, sizeof(esc_body), body) != 0)
        return 0;
    if (json_escape(esc_sub, sizeof(esc_sub), sub) != 0)
        return 0;
    iso_timestamp(timestamp, sizeof(timestamp));

    n = snprintf(buf, buflen, TOAST_TEMPLATE,
                 esc_body, esc_sub, esc_body, timestamp);
    if (n < 0 || (size_t)n >= buflen)
        return 0;
    return (size_t)n;
}

int notify_send(const char *body, const char *sub)
{
    static char payload[NOTIFY_PAYLOAD_MAX];

    if (notify_build_payload(payload, sizeof(payload), body, sub) == 0)
        return -1;

#ifdef SP_HOST
    fprintf(stderr, "notify: %s / %s\n", body, sub);
    return 0;
#else
    return sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
                               payload);
#endif
}
