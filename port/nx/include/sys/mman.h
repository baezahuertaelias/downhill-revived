/* wasm2c's runtime includes this for guard pages it does not use here: guest_memory_nx.c maps guest
   memory, and the build skips its signal recovery. */
#ifndef DHD_NX_MMAN_H
#define DHD_NX_MMAN_H
#endif
