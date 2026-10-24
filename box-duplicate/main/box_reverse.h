#ifndef BOX_REVERSE_H
#define BOX_REVERSE_H
#include <stddef.h>
#include <stdint.h>
#define BOX_REVERSE_ACK_CAPACITY 10
#define BOX_TEST_BOX_ID "10000000-0000-4000-8000-aabbccddee01"
/* All lifecycle hooks except build_acb run in NimBLE host context. */
void box_reverse_init(void (*resume_advertising)(void));
void box_reverse_on_sync(uint8_t own_addr_type);
void box_reverse_on_reset(void);
void box_reverse_peripheral_connected(uint16_t conn_handle);
void box_reverse_peripheral_disconnected(uint16_t conn_handle);
/* Thread safe: snapshots pending CBB/CDB IDs, never box IDs. Keeps them
 * for 60 seconds for retransmission; local TX is not an Android ACK. */
size_t box_reverse_build_acb(unsigned slot, char *out, size_t capacity);
#endif
