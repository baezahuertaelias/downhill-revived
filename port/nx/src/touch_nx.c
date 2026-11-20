/* The touch controls (port/host/touch_host.c) in place of the SDL events they read: the Switch has no
   touch controls yet, so this is that file's "no touch here" answer (a desktop's): the game's pad
   prompts apply, no overlay is drawn, nothing is merged into the pad. Reading the console's touch
   screen (libnx's hid) would replace this file. */
#include "../../hle/hle.h"
#include "../../host/host_api.h"

void touch_host_args(int argc, char **argv) { (void)argc; (void)argv; }  /* no --touch-editor */
int touch_host_event(const union SDL_Event *e) { (void)e; return 0; }
void touch_host_tick(void) {}
void touch_host_merge(HostPad *out) { (void)out; }
int touch_host_pad_prompts(void) { return 1; }
void touch_host_app_away(int away) { (void)away; }
void host_service_touch_field(uint32_t address) { (void)address; }
int touch_host_overlay(float *items, int max, float *width_mm, float *height_mm)
{
    (void)items; (void)max; (void)width_mm; (void)height_mm;
    return 0;
}
