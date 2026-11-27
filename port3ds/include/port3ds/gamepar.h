/* Game-side counters, the name index and the parallel map draw pass (docs/PORT_3DS_GAMEPAR.md).
 *
 * Step 0, switch gamecnt=N (port3ds/game/gamecnt.c): per-frame counts of the game's hot loops, logged as
 * [GAMECNT] lines every N game frames: map objects mapDisp walks and the dispEntry calls it makes, the name
 * lookups (mapGetMapObj, hitNameToPtr, lightNameToPtr) with their probe lengths and top callers, qqsort calls
 * by caller with their element counts, hitCheckVecFilter and animPoseMain calls. jobprobe=1 adds the round
 * trip of an empty job to each worker.
 *
 * Step 1, switch mapnameidx=1 (port3ds/game/nameidx.c): the three name lookups use a hash index instead of a
 * linear strcmp walk. The index keeps the items in walk order (entry, then joint), so it returns the first
 * match, as the walk does; the DELETED (hit) and ENABLED (light) flags are tested at lookup time on the
 * same-name candidates in that order. mapnameidx=2 also runs the walk and counts the lookups whose results
 * differ ([NAMEIDX] lines).
 *
 * Step 3, switch mapdispjob=1 (src/driver/mapdrv.c, TARGET_PORT): mapDisp computes the matrices first, then
 * each object's outcome (skip stage, depth, mode, dispEntry calls) in parallel (plat_job_pfor_dyn), then
 * applies the outcomes in object order. mapdispjob=2 also runs the original loop and compares its dispEntry
 * records and screen rectangles with the parallel pass ([MAPDISPJOB] lines).
 *
 * Every switch is off by default. None of this exists in the GameCube build. */
#ifndef PORT3DS_GAMEPAR_H
#define PORT3DS_GAMEPAR_H

#include <dolphin/types.h>

struct MapObject;
struct MapWork;
struct HitEntry;
struct LightEntry;
struct LightWork;

/* ---- Step 0: counters ---- */
extern int port_gamecnt; /* -1 unread, 0 off, else the log interval in frames */
int port_gamecnt_probe(void);
static inline int port_gamecnt_on(void) { return port_gamecnt > 0 || (port_gamecnt < 0 && port_gamecnt_probe() > 0); }
void port_cnt_qqsort(void* caller, u32 n);
void port_cnt_hitvec(void);
void port_cnt_animpose(void);
void port_cnt_mapdisp(u32 objects, u32 entries, u64 ticks);
void port_cnt_lookup_ticks(u64 ticks); /* nameidx.c: time in the three lookups */
/* Once per game frame (vi.c frame hooks). */
void port_gamepar_frame(u32 frame);
/* The name-lookup counters (nameidx.c reports each lookup here when gamecnt is on). kind: 0 map, 1 hit, 2 light. */
void port_cnt_lookup(int kind, void* caller, u32 probes, u32 compares);

/* ---- Step 1: name index ---- */
/* -1 unread; 0: the game's own walk; else the lookups go through port3ds/game/nameidx.c (mapnameidx, or gamecnt
 * for the probe counts of the walk). */
extern int port_nameidx_route;
int port_nameidx_probe(void);
static inline int port_nameidx_on(void) { return port_nameidx_route > 0 || (port_nameidx_route < 0 && port_nameidx_probe() > 0); }
struct MapObject* portMapGetMapObj(struct MapWork* wp, const char* name, void* caller);
struct HitEntry* portHitNameToPtr(struct MapWork* wp, const char* name, void* caller);
struct LightEntry* portLightNameToPtr(struct LightWork* wp, const char* name, void* caller);
/* The map objects, hit entries or lights may have changed (loads, unloads, mobj hit entries, light entries):
 * the indexes are rebuilt at their next lookup. */
void portNameIdxChanged(void);
void port_nameidx_frame(u32 frame);

/* ---- Step 3: mapDisp in phases (src/driver/mapdrv.c) ---- */
void portMapDispJobFrame(u32 frame); /* [MAPDISPJOB] totals every 120 frames */

/* ---- Step 6: early pose evaluation (src/driver/animdrv.c) ----
 * Switch animprejob=1: at the start of a window, the poses the previous window evaluated (animPoseMain, no paper
 * overlay, not a shared paper pose, float scratch off) are evaluated in place on job worker 0, each with a backup of
 * what it writes. The game's animPoseMain call reuses the result when its key (file, animation, time, pose clock and
 * flags, rotation, camera, buffers) is unchanged; any other driver access to the pose, and the window's end, restore
 * the backup first. animprejob=2: each reuse restores the backup, runs the call and compares scalars and buffers
 * ([ANIMPREJOB] lines). animprewin=0: the window is camDraw (field characters); 1: from marioStMain to the end of
 * camDraw (battle units are evaluated in btlDispMain, in the game logic). */
void portAnimPreBegin(int where); /* 0 camDraw start, 1 marioStMain start */
void portAnimPreEnd(void);        /* camDraw end */
void portAnimPreFrame(u32 frame); /* [ANIMPREJOB] totals every 120 frames */

#endif
