/* What the Rust library gives the other platforms and the Switch's first version goes without:
   online races (dhd_net, dhd_disc), Discord (dhd_presence) and the texture packs' PNGs. */
#include "dhd_disc.h"
#include "dhd_presence.h"
#include "dhd_render.h"
#include "../../net/ws.h"
#include <stdio.h>

int dhd_disc_check(const char *iso, const char *cache, uint8_t digest[32], char *note, size_t size)
{
    (void)iso; (void)cache; (void)digest;
    snprintf(note, size, "online is not in the Switch build yet");
    return -1;
}
int dhd_presence_start(const char *setting, DhdPresenceLog log) { (void)setting; (void)log; return 0; }
void dhd_presence_set(int kind, const char *track, int place, int riders, const char *room, int members, int max_members)
{
    (void)kind; (void)track; (void)place; (void)riders; (void)room; (void)members; (void)max_members;
}
void dhd_presence_stop(void) {}
WsConn *ws_start(const char *url, const WsHandler *h) { (void)url; (void)h; return NULL; }
void ws_send(WsConn *ws, int binary, const void *data, size_t n) { (void)ws; (void)binary; (void)data; (void)n; }
void ws_stop(WsConn *ws) { (void)ws; }
int http_get(const char *url, char *out, size_t cap) { (void)url; (void)out; (void)cap; return -1; }
int dhdr_png_size(const uint8_t *data, size_t size, uint32_t *w, uint32_t *h) { (void)data; (void)size; (void)w; (void)h; return 0; }
int dhdr_png_decode(const uint8_t *data, size_t size, uint8_t *rgba, uint32_t w, uint32_t h, uint32_t opaque)
{
    (void)data; (void)size; (void)rgba; (void)w; (void)h; (void)opaque;
    return 0;
}
