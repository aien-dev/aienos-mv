/* allen_intent.h -- How Allen reads a line he cannot answer by heart and
 * writes the job he hands to a far-off worker (a language model reached
 * through a courier, svc/courier.h). Allen decides what is wanted, something
 * made, something explained, something done, or just talk, and composes the
 * instruction himself: who he is, who asked, and the shape the answer must
 * take, so the worker speaks in his voice and the person only ever hears
 * Allen. Pure: no I/O, no model here. Host tested in ui/tests/allen_ui_test.c. */
#ifndef AIENOS_ALLEN_INTENT_H
#define AIENOS_ALLEN_INTENT_H
#include <stdint.h>
#include "allen_reply.h"

typedef enum { CK_WANT_CHAT, CK_WANT_MAKE, CK_WANT_EXPLAIN, CK_WANT_DO } ck_allen_want;

/* What a folded line (ck_allen_fold) asks for, from its first word and its verbs. */
ck_allen_want ck_allen_want_of(const char *folded);
/* "chat", "make", "explain" or "do". */
const char *ck_allen_want_name(ck_allen_want w);
/* The job Allen sends, one line: "JOB <seq> <want> <instruction>\t<line>"
 * (tabs, newlines and control bytes in line become spaces; no trailing
 * newline). Returns the length, 0 when cap is too small. */
unsigned ck_allen_job(const ck_allen_mind *m, uint32_t seq, const char *line, char *out, unsigned cap);
/* Reads an answer "SAY <seq> <text>" (the worker turns newlines into "\n"):
 * 1 with the seq and the text (at most cap - 1 bytes, "\n" back to a space),
 * 0 when the line is not an answer. */
int ck_allen_answer_line(const char *in, uint32_t *seq, char *text, unsigned cap);

#endif
