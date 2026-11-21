// Wait loops that spin until an interrupt-time callback sets a flag (ARQ completions in arammgr.c).
//
// On the GameCube the loop body is empty and the compiler re-reads the flag each time round. A port's compiler
// may keep the flag in a register (nothing in the loop says the memory can change), and its "interrupt" runs on
// another host thread: PORT_SPIN() is a compiler barrier plus a yield there, and nothing in the matching build.
#pragma once

#ifdef TARGET_PORT
void port_spin(void);
#define PORT_SPIN() port_spin()
#else
#define PORT_SPIN()
#endif
