/* The exact renderer (raster.wgsl's compute rasterizer): off on the Switch. Native mode needs only that it exists. */
#include "dhd_render.h"
#include "dhd_render_ext.h"
#include <stdlib.h>
#include <string.h>

DhdrExact *dhdr_exact_create(uint32_t raster_flags) { return (DhdrExact *)calloc(1, 16); }
void dhdr_exact_destroy(DhdrExact *x) {  }
void dhdr_exact_write(DhdrExact *x, uint32_t buffer, uint32_t offset, const void *data, uint32_t bytes) {  }
void dhdr_exact_copy(DhdrExact *x, uint32_t from, uint32_t from_offset, uint32_t to, uint32_t to_offset, uint32_t bytes) {  }
void dhdr_exact_batch(DhdrExact *x, uint32_t dynamic_offset, uint32_t groups_x, uint32_t groups_y) {  }
void dhdr_exact_record(DhdrExact *x, uint32_t pipeline, uint32_t x_word, uint32_t y_word) {  }
void dhdr_exact_submit(DhdrExact *x) {  }
int dhdr_exact_read_begin(DhdrExact *x, uint32_t buffer, const uint32_t want[16]) { return 1; }
int dhdr_exact_read_finish(DhdrExact *x, uint32_t buffer, const uint32_t want[16], void *image) { return 1; }
int dhdr_exact_capture_begin(DhdrExact *x, uint32_t *w, uint32_t *h) { return 0; }
int dhdr_exact_capture_finish(DhdrExact *x, uint32_t *pixels) { return 0; }
void dhdr_exact_stats(const DhdrExact *x, DhdrExactStats *out) {  }
int dhdr_present_exact(DhdrSurface *s, DhdrExact *x, const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t window_w, uint32_t window_h) { return 0; }
