#ifndef GALAXYVITA_SHADER_CACHE_H
#define GALAXYVITA_SHADER_CACHE_H

#include <psp2/gxm.h>

/* On-device Cg -> GXP compilation, with the result persisted so that only the
 * first run pays for it.
 *
 * There is no host GXP compiler: psp2cgc ships with Sony's SDK and the VitaSDK
 * does not have it. The compiler that does exist is the console's own,
 * libshacccg.suprx, which is firmware and not distributable -- the user
 * supplies it, exactly as they supply the ROM. So compilation happens here, on
 * the device, and the cache is what keeps it off the hot path.
 *
 * ShaccCg cannot survive an unbounded number of compiles in one process (see
 * docs/GRAPHICS-BACKEND.md), so gvsc_budget_left() lets a caller stop before
 * the wall and resume in the next launch rather than wedge. */

/* dir is where .gxp blobs live, e.g. ux0:data/galaxyvita/gxp.
 * Returns 1 if the runtime compiler is available, 0 if only the cache is.
 * A 0 is not fatal: everything already cached still loads. */
int gvsc_init(const char *dir);

/* Looks `name` up in the cache; compiles `src` into it on a miss. `src` may be
 * NULL, which makes this a cache-only lookup. Returns a program owned by the
 * cache -- do not free -- or NULL.
 *
 * `name` is the cache key and the file name, so it must be unique per source.
 * The Dolphin-generated shaders are keyed by their FNV-1a-64 UID; the built-in
 * ones by a fixed name plus a version suffix that must be bumped whenever the
 * source changes, or a stale blob would be loaded forever. */
const SceGxmProgram *gvsc_program(const char *name, const char *src, int fragment);

/* How many more compiles this process should attempt. Reaching 0 means the
 * caller should finish the launch and let the next one continue. */
int gvsc_budget_left(void);

/* Diagnostics for the last gvsc_program() that failed: the compiler's own
 * message, or a short explanation if it never got that far. */
const char *gvsc_last_error(void);

/* compiles, cache hits, failures -- for the boot log. */
void gvsc_stats(unsigned *compiled, unsigned *cached, unsigned *failed);

void gvsc_shutdown(void);

#endif
