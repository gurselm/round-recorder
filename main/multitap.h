#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Typing on a phone keypad, for the round screen's twelve big keys: pressing
 * a key again within MULTITAP_MS steps through its letters, replacing the one
 * just typed; another key, or a pause, keeps it. The first letter of the text
 * comes out capital. No IDF in here, so the host tests compile it.
 */

#define MULTITAP_MS 900
#define MULTITAP_KEYS 10   /* 0 is space, 1 punctuation, 2 to 9 letters */

/* What the keys type. Numbers and symbols take one press a character; the
 * digits go 1 to 9, 0 round the ring from the top, the key abc is on first. */
typedef enum {
    MULTITAP_LETTERS,
    MULTITAP_NUMBERS,
    MULTITAP_SYMBOLS,
    MULTITAP_MODES,
} multitap_mode_t;

typedef struct {
    int key;          /* the key pressed last, or -1 */
    int64_t at_ms;
    int step;         /* which of its characters that press gave */
    multitap_mode_t mode;
} multitap_t;

/* Letters, nothing pressed yet: a fresh start. */
void multitap_reset(multitap_t *m);
/* The next press starts a new character, in the same mode (after a delete). */
void multitap_break(multitap_t *m);
void multitap_set_mode(multitap_t *m, multitap_mode_t mode);

/* What key shows in the mode it's in: its letters, its digit, its symbol. */
const char *multitap_label(const multitap_t *m, int key);

/* A press of key at now_ms, on text len characters long: the character to
 * type, and in *replace whether it takes the place of the last one. */
char multitap_press(multitap_t *m, int key, int64_t now_ms, int len, bool *replace);
