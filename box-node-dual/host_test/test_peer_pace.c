/* Хост-тест peer_pace.c (ASan/UBSan). */
#include <assert.h>
#include <stdio.h>

#include "peer_pace.h"

static int checks;
#define CHECK(c) do { assert(c); checks++; } while (0)

int main(void)
{
    pp_t p = {0};
    CHECK(!pp_quiet(&p, 0) && pp_left_ms(&p, 123) == 0);

    /* дубликаты подряд: 30, 60, 120, 120 с */
    CHECK(pp_quiet_ms(0) == 0 && pp_quiet_ms(1) == 30000 && pp_quiet_ms(2) == 60000);
    CHECK(pp_quiet_ms(3) == 120000 && pp_quiet_ms(4) == 120000 && pp_quiet_ms(255) == 120000);

    uint32_t t = 1000;
    pp_on_session(&p, false, t);
    CHECK(p.idle == 1 && pp_quiet(&p, t + 29999) && !pp_quiet(&p, t + 30001));
    t += 31000;
    pp_on_session(&p, false, t);
    CHECK(p.idle == 2 && pp_quiet(&p, t + 59000) && !pp_quiet(&p, t + 60001));
    t += 61000;
    pp_on_session(&p, false, t);
    CHECK(p.idle == 3 && pp_left_ms(&p, t) >= 119999 && pp_left_ms(&p, t) <= 120001);

    /* новое сообщение снимает паузу */
    pp_on_session(&p, true, t + 1);
    CHECK(p.idle == 0 && p.until == 0 && !pp_quiet(&p, t + 2));

    /* переполнение часов (uint32 мс, ~49 дней) */
    pp_t q = {0};
    uint32_t w = 0xFFFFFFFFu - 1000u;
    pp_on_session(&q, false, w);
    CHECK(pp_quiet(&q, w + 500) && pp_quiet(&q, w + 20000) && !pp_quiet(&q, w + 31000));

    /* счётчик не переполняется */
    pp_t r = {0};
    for (int i = 0; i < 300; i++) pp_on_session(&r, false, (uint32_t)i * 200000u);
    CHECK(r.idle == 255 && pp_quiet_ms(r.idle) == PP_MAX_MS);

    printf("test_peer_pace: %d checks OK\n", checks);
    return 0;
}
