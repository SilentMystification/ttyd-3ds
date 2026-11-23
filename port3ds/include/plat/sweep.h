/* Map sweep entry hooks (switch sweep=N, tools/port3ds/map_sweep.py).
 *
 * The sweep visits a list of maps and, for a name that is not a map-change
 * target, asks the game which map to enter it through. The generic sweep code
 * knows no map names: the game registers one hook at sweep start, and the sweep
 * calls it for every name the disc's map table does not hold.
 *
 * Plain C, no game types, so the plat side and the game side can both use it. */
#ifndef PLAT_SWEEP_H
#define PLAT_SWEEP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Given a sweep name, return the host map to enter it through and set *story
 * to the story value that host needs; return NULL if the hook does not handle
 * the name. The returned string must stay valid for the rest of the run. */
typedef const char* (*SweepEntryHook)(const char* map, int* story);

/* Register the hook (NULL removes it). Replaces any earlier registration. */
void plat_sweep_register_entry_hook(SweepEntryHook hook);

/* Host map for a sweep name, or NULL when no hook is registered or the hook
 * declines the name. */
const char* plat_sweep_entry_host(const char* map, int* story);

#ifdef __cplusplus
}
#endif

#endif
