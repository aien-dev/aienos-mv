/* allen_reply.c -- Allen's first local answers; see allen_reply.h. */
#include "allen_reply.h"

#include <stddef.h>

static const struct {
    const char *heard; /* folded: lowercase letters and digits, single spaces */
    const char *answer;
} known[] = {
    {"hello allen", "Hello Humanity."},
    {"hi allen", "Hello Humanity."},
    {"hey allen", "Hello Humanity."},
    {"hello", "Hello. I am Allen."},
    {"hi", "Hello. I am Allen."},
    {"who are you", "I am Allen. I live on this iMac."},
    {"where are you", "Here, on this iMac. The Spark is not connected."},
};

/* Lowercases letters, keeps digits, turns every other run into one space,
 * trims. Returns the folded length (at most cap - 1). */
static size_t fold(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    int space = 0;
    for (; *s && n + 1 < cap; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (space && n)
                out[n++] = ' ';
            if (n + 1 < cap)
                out[n++] = c;
            space = 0;
        } else {
            space = 1;
        }
    }
    out[n] = 0;
    return n;
}

const char *ck_allen_reply(const char *line)
{
    char f[96];
    fold(line ? line : "", f, sizeof f);
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        const char *a = known[i].heard, *b = f;
        while (*a && *a == *b)
            a++, b++;
        if (!*a && !*b)
            return known[i].answer;
    }
    return "I don't know that yet.";
}
