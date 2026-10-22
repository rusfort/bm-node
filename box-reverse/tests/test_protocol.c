#include "box_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ID "10000000-0000-4000-8000-aabbccddeeff"
#define CB "30000000-0000-4000-8000-112233445566"
static unsigned tests;
static void check(const char *s, bool expected, box_packet_type_t t, size_t n)
{
    box_packet_t p;
    bool ok = box_protocol_parse(s, strlen(s), &p);
    assert(ok == expected);
    if (ok) { assert(p.type == t); assert(p.box_count == n); }
    ++tests;
}
int main(void)
{
    check("V1\nSID\n" ID, true, BOX_PACKET_SID, 0);
    check("V1\nTRM\n" ID "\n", true, BOX_PACKET_TRM, 0);
    check("V1\nCBB\n" CB "\n" ID, true, BOX_PACKET_CBB, 1);
    check("V1\nCDB\n" CB "\n" ID, true, BOX_PACKET_CDB, 1);
    check("V1\nCBB\n" CB "\n" ID "\n" ID, true, BOX_PACKET_CBB, 2);
    check("V2\nCBB\n" CB "\n" ID, false, 0, 0);
    check("V1\nCBB\n" CB, false, 0, 0);
    check("V1\nCBB\nwrong\n" ID, false, 0, 0);
    check("V1\nCBB\n" CB "\nwrong", false, 0, 0);
    check("V1\nCDB\n" CB "\n\n" ID, false, 0, 0);
    check("V1\nSID\n" ID "\nextra", false, 0, 0);
    check("V1\nSID\n" ID "\r\n", false, 0, 0);
    check("V1\nACB\n" CB, true, BOX_PACKET_OTHER, 0);
    box_packet_t p;
    const char nul[] = "V1\nSID\n" ID "\0bad";
    assert(!box_protocol_parse(nul, sizeof(nul) - 1, &p)); ++tests;
    char large[514]; memset(large, 'a', sizeof(large));
    assert(!box_protocol_parse(large, 513, &p)); ++tests;
    char list[513] = "V1\nCBB\n" CB;
    for (int i = 0; i < 10; ++i) strcat(list, "\n" ID);
    check(list, true, BOX_PACKET_CBB, 10);
    strcat(list, "\n" ID); check(list, false, 0, 0);
    assert(!box_protocol_uuid_valid("10000000-0000-4000-8000-aabbccddeefg")); ++tests;
    assert(!box_protocol_parse(NULL, 3, &p)); ++tests;
    printf("PASS: %u protocol tests\n", tests);
}
