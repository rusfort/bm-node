#include "peer_pace.h"

uint32_t pp_quiet_ms(uint8_t idle)
{
    if (idle == 0) return 0;
    uint32_t ms = PP_FIRST_MS;
    for (uint8_t i = 1; i < idle && ms < PP_MAX_MS; i++) ms *= 2;
    return ms > PP_MAX_MS ? PP_MAX_MS : ms;
}

void pp_on_session(pp_t *p, bool useful, uint32_t now)
{
    if (useful) { p->idle = 0; p->until = 0; return; }
    if (p->idle < 255) p->idle++;
    p->until = (now + pp_quiet_ms(p->idle)) | 1u;   /* 0 зарезервирован под «нет паузы» */
}

bool pp_quiet(const pp_t *p, uint32_t now)
{
    return p->until != 0 && (int32_t)(p->until - now) > 0;
}

uint32_t pp_left_ms(const pp_t *p, uint32_t now)
{
    return pp_quiet(p, now) ? p->until - now : 0;
}
