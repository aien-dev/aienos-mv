/* allen_reply.h -- Allen's local conversation on the iMac: greetings, small
 * talk, what it is and where it lives, facts about the machine it runs on,
 * your name (for this session), how long it has been awake, and simple
 * arithmetic said in words. Lines are matched after folding case,
 * punctuation and spacing; anything else gets an honest "not yet". No model,
 * no network: every answer comes from the iMac alone. Nothing here outlives
 * a restart yet. Pure, host tested in ui/tests/allen_ui_test.c. */
#ifndef AIENOS_ALLEN_REPLY_H
#define AIENOS_ALLEN_REPLY_H
#include <stdint.h>

/* What Allen knows about the machine (filled by the kernel; 0 = unknown). */
typedef struct {
    unsigned cores, sata_disks, nvme_disks;
    unsigned screen_w, screen_h;
} ck_allen_facts;

/* One conversation: what Allen has been told and asked so far. */
typedef struct {
    ck_allen_facts facts;
    char name[24];  /* the person's name once told, "" before */
    unsigned turns; /* lines answered so far */
    int asked_how;  /* Allen just asked "how are you?" */
    char out[160];  /* the last composed answer */
} ck_allen_mind;

void ck_allen_mind_init(ck_allen_mind *m, const ck_allen_facts *facts);
/* Allen's answer to one typed line (never NULL; valid until the next call).
 * awake_s: seconds since the machine started. */
const char *ck_allen_answer(ck_allen_mind *m, const char *line, uint64_t awake_s);
/* Stateless: the answer of a fresh conversation at 0 s awake. */
const char *ck_allen_reply(const char *line);
/* The answer to "goodbye" (and "bye", with or without "allen"); the caller
 * compares the pointer to end the session. */
extern const char ck_allen_goodbye[];

#endif
