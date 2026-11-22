/* Battle test hook, shared by the 3DS port and the GameCube debug DOL (C89).
 *
 * btlauto=perfect|fail  runs battles unattended:
 *   - while the command menu is up (BATTLE_DISP_FLAG_COMMAND_MENU) and no
 *     action command runs, it presses the keys of the current turn's entry in
 *     btlmoves (default: A, which takes the menu's default: the first action,
 *     its first move, the first target), one key every btlstep=N frames
 *     (default 10, held 2 frames);
 *   - perfect: Mario's side carries the Auto Command badge (the game's own
 *     debug badge: every action command and guard succeeds, guards are
 *     Superguards where the move allows them);
 *   - fail: the badge is taken away and no key is pressed while an action
 *     command or an enemy's move runs, so every command fails;
 *   - during the end of the battle (star points, a level-up) it presses A.
 * btlmoves=<turn>,<turn>,...  one entry per command menu opening: a
 *   "."-separated key list (a b x y l r z start up down left right), for
 *   example "right.a.a" (the next action in the menu, then its first move and
 *   target). After the list, A only.
 * Logs "btlauto: turn T unit U menu", the moves and "btlauto: action command
 * ... result R".
 */
#include <dolphin/pad.h>
#include <string.h>

#include "battle/battle.h"
#include "battle/battle_ac.h"
#include "battle/battle_unit.h"
#include "driver/seqdrv.h"
#include "mario/mariost.h"
#include "test_mode.h"

#define MOVES_MAX 32
#define KEYS_MAX 12

static int s_state = -1; /* -1 unread, 0 off, 1 perfect, 2 fail */
static char s_moves[MOVES_MAX][KEYS_MAX];
static int s_moves_len[MOVES_MAX];
static int s_moves_n, s_turn;
static int s_step = 10;
static int s_menu_was_up;
static u32 s_menu_frame;
static int s_ac_was_on;

static u32 key_bit(const char* k, int n) {
    static const struct {
        const char* name;
        u32 bit;
    } keys[] = {
        {"a", PAD_BUTTON_A},         {"b", PAD_BUTTON_B},         {"x", PAD_BUTTON_X},
        {"y", PAD_BUTTON_Y},         {"l", PAD_TRIGGER_L},        {"r", PAD_TRIGGER_R},
        {"z", PAD_TRIGGER_Z},        {"start", PAD_BUTTON_START}, {"up", PAD_BUTTON_UP},
        {"down", PAD_BUTTON_DOWN},   {"left", PAD_BUTTON_LEFT},   {"right", PAD_BUTTON_RIGHT},
    };
    u32 i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if ((int)strlen(keys[i].name) == n && strncmp(keys[i].name, k, n) == 0) {
            return keys[i].bit;
        }
    }
    tst_log("btlauto: unknown key %.*s", n, k);
    return 0;
}

static void read_switches(void) {
    char v[16];
    char text[256];
    char* p;
    s_state = 0;
    if (!tst_sw_value("btlauto", v, sizeof(v))) {
        return;
    }
    s_state = strcmp(v, "fail") == 0 ? 2 : 1;
    s_step = tst_sw_int("btlstep", 10);
    if (s_step < 3) {
        s_step = 3;
    }
    if (tst_sw_value("btlmoves", text, sizeof(text))) {
        p = text;
        while (*p != '\0' && s_moves_n < MOVES_MAX) {
            int n = 0;
            while (*p != '\0' && *p != ',') {
                const char* k = p;
                while (*p != '\0' && *p != ',' && *p != '.') {
                    p++;
                }
                if (p > k && n < KEYS_MAX) {
                    /* stored as the bit number + 1 (0: no key) */
                    u32 bit = key_bit(k, (int)(p - k));
                    int b;
                    s_moves[s_moves_n][n] = 0;
                    for (b = 0; b < 16; b++) {
                        if (bit == (1u << b)) {
                            s_moves[s_moves_n][n] = (char)(b + 1);
                        }
                    }
                    n++;
                }
                if (*p == '.') {
                    p++;
                }
            }
            s_moves_len[s_moves_n++] = n;
            if (*p == ',') {
                p++;
            }
        }
    }
    tst_log("btlauto: %s, %d turn entries, a key every %d frames", s_state == 1 ? "perfect" : "fail", s_moves_n, s_step);
}

/* The key to press now in this menu opening, or 0. */
static u32 menu_key(u32 frame) {
    u32 since = frame - s_menu_frame;
    int i;
    if (since < (u32)s_step || since % (u32)s_step >= 2) {
        return 0;
    }
    i = (int)(since / (u32)s_step) - 1;
    if (s_turn - 1 < s_moves_n && s_turn >= 1) {
        int t = s_turn - 1;
        if (i < s_moves_len[t]) {
            int b = s_moves[t][i];
            return b > 0 ? 1u << (b - 1) : 0;
        }
    }
    return PAD_BUTTON_A;
}

void tst_battle_frame(u32 frame) {
    BattleWork* wp;
    int menu_up, ac_on, i;
    u32 keys = 0;

    if (s_state < 0) {
        read_switches();
    }
    if (s_state <= 0) {
        return;
    }
    wp = _battleWorkPointer;
    if (seqGetSeq() != SEQ_BATTLE || wp == NULL || !gp->inBattle) {
        s_menu_was_up = 0;
        s_ac_was_on = 0;
        tst_set_keys(0);
        return;
    }
    for (i = 0; i < BATTLE_UNIT_MAX; i++) {
        BattleUnit* u = wp->units[i];
        if (u != NULL && u->alliance == 0) {
            u->badgesEquipped.AutoCommand = s_state == 1 ? 1 : 0;
        }
    }
    menu_up = (wp->dispFlags & BATTLE_DISP_FLAG_COMMAND_MENU) != 0;
    ac_on = wp->actionCommands.maincb != NULL;
    if (menu_up && !s_menu_was_up) {
        s_turn++;
        s_menu_frame = frame;
        tst_log("btlauto: turn %d (battle turn %d) unit %ld menu at frame %lu", s_turn, (int)wp->turnCount,
                (long)wp->activeUnitId, (unsigned long)frame);
    }
    s_menu_was_up = menu_up;
    if (ac_on && !s_ac_was_on) {
        tst_log("btlauto: action command starts at frame %lu", (unsigned long)frame);
    } else if (!ac_on && s_ac_was_on) {
        tst_log("btlauto: action command ends at frame %lu, result %ld", (unsigned long)frame,
                (long)BattleActionCommandResult(wp));
    }
    s_ac_was_on = ac_on;
    if (menu_up && !ac_on) {
        keys = menu_key(frame);
    } else if (wp->seqEndWork != NULL && !ac_on && frame % 10 < 2) {
        keys = PAD_BUTTON_A;
    }
    tst_set_keys(keys);
}
