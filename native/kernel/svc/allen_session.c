/* allen_session.c -- Allen in the AArch64 console session (CK_ALLEN=1).
 *
 * The console session (dev/usb_kbd.c) hands every finished line here instead
 * of the shell. Allen answers with the same local conversation as on every
 * other machine (svc/allen_reply.c); the console already mirrors to the
 * screen (core/console.c), so the answer shows on serial and on the display.
 * "goodbye" ends the session. Nothing on screen names the machine: Allen
 * only knows counts (svc/allen_reply.h facts).
 *
 * Memory: without a region (ck_allen_session_attach not called, or its disk
 * refused) the conversation lasts until power off, and Allen says so
 * through mind.lasting = 0. */
#include "allen_reply.h"
#include "allen_memory.h"
#include "../include/ck.h"

#if defined(CK_ALLEN) && CK_ALLEN

static ck_allen_mind s_mind;
static ck_allen_store s_store;
static int s_ready, s_store_ok;

void ck_allen_session_attach(const disk_dev *part)
{
    if (!part) return;
    ck_allen_memory mem;
    int rc = ck_allen_store_load(&s_store, part, &mem);
    ck_printf("allen_memory: %s rc=%d\n", rc == 0 ? "loaded" : "unavailable", rc);
    if (rc == 0) {
        s_mind.mem = mem;
        s_store_ok = 1;
    }
}

void ck_allen_session_begin(const ck_allen_facts *facts)
{
    ck_allen_memory keep = s_mind.mem;
    ck_allen_mind_init(&s_mind, facts);
    if (s_store_ok) {
        s_mind.mem = keep;
        s_mind.lasting = 1;
    }
    s_ready = 1;
    const char *hello = ck_allen_welcome(&s_mind); /* NULL: a first meeting, nothing to say yet */
    if (hello) ck_printf("allen: %s\n", hello);
    if (s_mind.lasting && s_mind.changed && ck_allen_store_save(&s_store, &s_mind.mem)) s_mind.lasting = 0;
    s_mind.changed = 0;
}

/* 1 when Allen said goodbye (the session ends). */
int ck_allen_session_line(const char *line)
{
    if (!s_ready) return 0;
    const char *reply = ck_allen_answer(&s_mind, line, ck_time_us() / 1000000u);
    int save = s_mind.changed || reply == ck_allen_goodbye;
    if (s_mind.lasting && save && ck_allen_store_save(&s_store, &s_mind.mem)) {
        s_mind.lasting = 0;
        if (s_mind.changed) reply = ck_allen_save_failed;
    }
    s_mind.changed = 0;
    s_mind.note_dirty = 0;
    s_mind.map_changed = 0;
    ck_printf("allen: %s\n", reply);
    return reply == ck_allen_goodbye;
}

#endif
