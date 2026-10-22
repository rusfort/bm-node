#ifndef BOX_PROTOCOL_H
#define BOX_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#define BOX_PROTOCOL_MAX_PACKET 512
#define BOX_PROTOCOL_MAX_BOX_IDS 10
typedef enum {
    BOX_PACKET_SID, BOX_PACKET_TRM, BOX_PACKET_CBB, BOX_PACKET_CDB,
    BOX_PACKET_OTHER
} box_packet_type_t;
typedef struct {
    box_packet_type_t type;
    char id[37];
    size_t box_count;
    char box_ids[BOX_PROTOCOL_MAX_BOX_IDS][37];
} box_packet_t;
bool box_protocol_uuid_valid(const char *s);
bool box_protocol_parse(const void *data, size_t len, box_packet_t *out);
#endif
