#include "box_protocol.h"
#include <string.h>

bool box_protocol_uuid_valid(const char *s)
{
    if (s == NULL || strlen(s) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool box_protocol_parse(const void *data, size_t len, box_packet_t *out)
{
    if (!data || !out || len == 0 || len > BOX_PROTOCOL_MAX_PACKET ||
        memchr(data, '\0', len) != NULL) return false;
    char buf[BOX_PROTOCOL_MAX_PACKET + 1];
    memcpy(buf, data, len);
    buf[len] = '\0';
    /* Java-generated control packets have no final newline; accept one. */
    if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    memset(out, 0, sizeof(*out));
    out->type = BOX_PACKET_OTHER;
    char *fields[3 + BOX_PROTOCOL_MAX_BOX_IDS];
    size_t count = 0;
    char *cur = buf;
    while (true) {
        if (count == sizeof(fields) / sizeof(fields[0])) return false;
        fields[count++] = cur;
        char *nl = strchr(cur, '\n');
        if (!nl) break;
        *nl = '\0';
        cur = nl + 1;
    }
    if (count < 2 || strcmp(fields[0], "V1")) return false;
    if (!strcmp(fields[1], "SID")) out->type = BOX_PACKET_SID;
    else if (!strcmp(fields[1], "TRM")) out->type = BOX_PACKET_TRM;
    else if (!strcmp(fields[1], "CBB")) out->type = BOX_PACKET_CBB;
    else if (!strcmp(fields[1], "CDB")) out->type = BOX_PACKET_CDB;
    else return true;
    if (count < 3 || !box_protocol_uuid_valid(fields[2])) return false;
    memcpy(out->id, fields[2], 37);
    if (out->type == BOX_PACKET_SID || out->type == BOX_PACKET_TRM)
        return count == 3;
    if (count < 4) return false;
    for (size_t i = 3; i < count; ++i) {
        if (!box_protocol_uuid_valid(fields[i])) return false;
        memcpy(out->box_ids[out->box_count++], fields[i], 37);
    }
    return true;
}
