#include "multitap.h"

#include <ctype.h>
#include <string.h>

static const char *const CHARS[MULTITAP_KEYS] = {
    " 0", ".,?!-'1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9",
};
static const char *const LETTER_LABELS[MULTITAP_KEYS] = {
    "space", ".,?", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz",
};
/* By key, so that round the ring, where the keys go 2 to 9, 0, 1 clockwise
 * from the top (abc first), the digits read 1 to 9, 0 clockwise from the top. */
static const char *const DIGITS[MULTITAP_KEYS] = { "9", "0", "1", "2", "3", "4", "5", "6", "7", "8" };
/* The ones a name might want that the letters' punctuation key hasn't got. */
static const char *const SYMBOLS[MULTITAP_KEYS] = { "_", "&", "+", "#", "@", "(", ")", "/", ":", "%" };

void multitap_break(multitap_t *m)
{
    m->key = -1;
    m->at_ms = 0;
    m->step = 0;
}

void multitap_reset(multitap_t *m)
{
    multitap_break(m);
    m->mode = MULTITAP_LETTERS;
}

void multitap_set_mode(multitap_t *m, multitap_mode_t mode)
{
    multitap_break(m);
    m->mode = mode < MULTITAP_MODES ? mode : MULTITAP_LETTERS;
}

const char *multitap_label(const multitap_t *m, int key)
{
    if (key < 0 || key >= MULTITAP_KEYS) {
        return "";
    }
    return m->mode == MULTITAP_NUMBERS ? DIGITS[key] : m->mode == MULTITAP_SYMBOLS ? SYMBOLS[key] : LETTER_LABELS[key];
}

char multitap_press(multitap_t *m, int key, int64_t now_ms, int len, bool *replace)
{
    *replace = false;
    if (key < 0 || key >= MULTITAP_KEYS) {
        return '\0';
    }
    if (m->mode != MULTITAP_LETTERS) {
        multitap_break(m);   /* one press, one character */
        return m->mode == MULTITAP_NUMBERS ? DIGITS[key][0] : SYMBOLS[key][0];
    }
    /* Again, soon, with something typed to step from: the next character. */
    if (key == m->key && now_ms - m->at_ms < MULTITAP_MS && len > 0) {
        m->step = (m->step + 1) % (int)strlen(CHARS[key]);
        *replace = true;
    } else {
        m->step = 0;
    }
    m->key = key;
    m->at_ms = now_ms;
    char c = CHARS[key][m->step];
    /* The text's first character: where a replacement goes, or a new one. */
    bool first = *replace ? len == 1 : len == 0;
    return first ? (char)toupper((unsigned char)c) : c;
}
