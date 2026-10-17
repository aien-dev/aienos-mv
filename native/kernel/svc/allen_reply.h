/* allen_reply.h -- Allen's first local answers on the iMac: a small fixed set
 * of phrases it knows, matched after folding case, punctuation and spacing;
 * anything else gets an honest "not yet". No model, no network: these answers
 * come from the iMac alone. Pure, host tested in ui/tests/allen_ui_test.c. */
#ifndef AIENOS_ALLEN_REPLY_H
#define AIENOS_ALLEN_REPLY_H

/* Allen's answer to one typed line (never NULL; a static string). */
const char *ck_allen_reply(const char *line);

#endif
