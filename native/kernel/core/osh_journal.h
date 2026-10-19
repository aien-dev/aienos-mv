/* osh_journal.h -- the durable effect journal of the AIENOS OSH adapter (aienos-mv#431, aien-architecture#158).
 *
 * Contract: aien-protocols-mv specs/osh-platform/OSH_PLATFORM_ABI.md v1 DRAFT section 10 and 13 (read at main
 * fb5fca98e667) and STREAM_TASK.md revision 2 ST8 (branch feat/osh-stream-abi-43 at 181855c9ff3e), whose rules win where
 * the two differ. Reference: the Linux host adapter's journal, omega src/osh/host/osh_journal.h and osh_exec.c at
 * a8ffd2b912c8 (INTENT before each effect, OUTCOME after, OUTCOME_UNKNOWN on recovery written once, never replayed,
 * mandatory journal fails closed, no argument values recorded).
 *
 * WHERE THE RECORDS GO. No second ledger: every record is one object of the canonical Omega Cortex
 * (third_party/omega/rx_cortex.h, M20: "every authoritative Cortex write goes through cx_append"), so the chain, the
 * per-record digests and the replay checks at open are Cortex's, unchanged. On AIENOS the Cortex journal backend is the
 * sealed Store (svc/cortex_journal.c, ck_cx_journal_store): one append is one sealed transaction, on the disk entirely
 * or not at all, durable when cx_append returns. That is the only backend this layer calls durable. The RAM backend
 * below (TEST only) keeps the same bytes in memory: it lets the host test and the QEMU gate's older cases drive the
 * same code, and its records are labelled durable=0 everywhere; it never counts as durable acceptance.
 *
 * RECORDS (Cortex payload words, little-endian u64; header subject OSHJ_SUBJECT, t = record count + 1, generation =
 * session epoch, protect CX_PROT_EFFECT_RECEIPT | CX_PROT_AUTHORITY so no operation may summarize them away):
 *   INTENT    (class CX_PLAN)      before the effect: what it is and what binds it (struct oshj_effect)
 *   STARTED   (class CX_EXECUTION) the provisional result: the effect was observed to begin (spawn returned a task,
 *                                  a write-class open happened); links[0] = the intent
 *   OUTCOME   (class CX_EXECUTION) the finished result, after observed completion: NOT_STARTED, COMPLETED(status),
 *                                  FAILED_NO_EFFECT(error), CANCELLED, OUTCOME_UNKNOWN; links[0] = the intent;
 *                                  recovered=1 when written by recovery (always OUTCOME_UNKNOWN)
 *   RECEIPT   (class CX_EVIDENCE)  the immutable pipeline receipt, last: the pipeline status and the ids of every
 *                                  outcome of the pipeline with a SHA-256 over their Cortex digests; recovered=1 when
 *                                  written by recovery
 *   RECONCILE (class CX_EVIDENCE)  an operator's explicit statement about an OUTCOME_UNKNOWN effect (the operator saw
 *                                  it happen, saw it not happen, or accepts it as unknown); links[0] = the intent,
 *                                  links[1] = the unknown outcome. It runs nothing and changes no earlier record.
 * Nothing transient is recorded as identity (STREAM_TASK ST3.3): no task id, pipe handle or address. A pipeline is
 * (epoch, seq); an effect is its intent's Cortex id. Redaction (section 13, Omega's rule): argv[0] and the argument
 * count only, never argument values, assignment values or stream contents; a location appears only as the SHA-256 of
 * its name and of its bytes.
 *
 * READ-ONLY INSPECTION AND RECOVERY (ABI section 9 rule 6, 10). oshj_inspect works on any Cortex store, also one opened
 * CX_OPEN_READONLY (whose appends Cortex itself refuses), and needs nothing of the OSH adapter: it does not include
 * osh_op.h and this file links no task, pipe or shell code, so it still runs when OSH itself cannot. oshj_recover
 * writes one OUTCOME_UNKNOWN (recovered=1) per open intent and one RECEIPT (recovered=1) per pipeline left without one;
 * it cannot launch anything (it has no reference to any launcher). A second recovery finds nothing open.
 *
 * UNVERIFIED (the drafts name the records but pin no encoding; each is this layer's own choice):
 *   J1 record kinds 0x4f5301..0x4f5305 (writer kinds below CX_K_BASE), the classes above, subject 0 (the owner, the
 *      principal the session acts for), generation = session epoch, the payload layouts below (version word
 *      OSHJ_MAGIC).
 *   J2 the STARTED record (the drafts have intent and outcome only); recovery uses it to split OUTCOME_UNKNOWN into
 *      "intent only" (the effect may not have begun) and "started" (it began; its end is unknown).
 *   J3 a refusal before the intent writes no journal record, only its security receipt line (Omega writes an outcome
 *      with ref 0); an effect refused at its own boundary after the intent gets FAILED_NO_EFFECT.
 *   J4 recovery writes status 70 with OUTCOME_UNKNOWN; reconciliation verdicts 1..3 and the capability that allows them
 *      (OSH_RES_RECONCILE, see osh_op.h).
 *   J5 the request digest is the unkeyed SHA-256 of #430 (Omega keys it with a per-journal HMAC key).
 */
#ifndef AIENOS_CK_OSH_JOURNAL_H
#define AIENOS_CK_OSH_JOURNAL_H

#include <stddef.h>
#include <stdint.h>

#include "../third_party/omega/rx_cortex.h"

#define OSHJ_MAGIC 0x4f53484a00000001ull /* "OSHJ", record layout version 1 */
#define OSHJ_SUBJECT 0u                  /* = CK_CX_FACT_SUBJECT (svc/cortex_journal.h), checked in svc/store_boot.c */
#define OSHJ_SUBJECTS 2u                 /* = CK_CX_SUBJECTS: the one Cortex of this subject */
#define OSHJ_NAME "cortex"               /* the journal name every AIENOS Cortex writer opens */
enum { OSHJ_K_INTENT = 0x4f5301u, OSHJ_K_STARTED, OSHJ_K_OUTCOME, OSHJ_K_RECEIPT, OSHJ_K_RECONCILE };

/* effects (section 10): the same values as osh_op.h's enum osh_effect */
enum { OSHJ_EF_CONSOLE = 0, OSHJ_EF_SPAWN = 1, OSHJ_EF_OPEN_WRITE = 2 };
/* outcomes (section 10): the same values as osh_op.h's enum osh_outcome */
enum { OSHJ_O_NOT_STARTED = 0, OSHJ_O_COMPLETED, OSHJ_O_FAILED_NO_EFFECT, OSHJ_O_CANCELLED, OSHJ_O_OUTCOME_UNKNOWN };
/* STARTED.what */
enum { OSHJ_ST_SPAWNED = 1, OSHJ_ST_OPENED = 2 };
/* RECONCILE.verdict (UNVERIFIED J4) */
enum { OSHJ_V_OBSERVED_DONE = 1, OSHJ_V_OBSERVED_ABSENT = 2, OSHJ_V_ACCEPT_UNKNOWN = 3 };
#define OSHJ_NAME_MAX 24u
#define OSHJ_RECEIPT_MAX 16u /* outcomes one receipt lists (= OSH_INTENT_MAX) */

/* What an INTENT binds (section 9 rule 2 binding, plus the identities "as available"). */
struct oshj_effect {
    uint8_t effect, cmd, ncmds, argc;
    uint8_t mode;              /* write-class open: 2 create or truncate, 3 append; else 0 */
    uint32_t seq, epoch;       /* the pipeline: 1-based number in the session, session epoch */
    uint8_t digest[32];        /* request digest */
    uint8_t principal[32];     /* the operator or agent principal the binding names */
    uint8_t domain;            /* binding: 1 = kernel IPC table */
    uint16_t resource_class, operation;
    uint32_t cap_index;
    uint64_t cap_generation;
    uint8_t stage_id[32];      /* spawn: the admitted unit's digest (zero: unknown) */
    uint8_t input_hash[32];    /* spawn whose stdin is a location: SHA-256 of its bytes at the intent; else zero */
    uint8_t target_hash[32];   /* the location written (the open, or a spawn's stdout): SHA-256 of its name; else zero */
    uint8_t target_pre[32];    /* write-class open: SHA-256 of the location's bytes before the effect; else zero */
    uint8_t name_len;
    uint8_t name[OSHJ_NAME_MAX]; /* argv[0] (cut to OSHJ_NAME_MAX): a lookup key, never authority */
};

struct osh_journal {
    CxStore cx;
    uint8_t open, durable, poisoned, readonly;
    const char *label;     /* "sealed_store", "ram_test" */
    uint32_t appends, failed;
    uint32_t max_epoch;    /* the highest epoch recorded when opened */
    int last_rc;           /* the last refused open or append (CX_ERR_*) */
};

/* Open the Cortex journal OSHJ_NAME through `ops`. durable: 1 only for a backend whose append is durable on return
 * (the sealed Store). ro: CX_OPEN_READONLY (inspection; appends refused by Cortex). 0 = CX_OK, else CX_ERR_*; on error
 * the journal is closed and every append refused. A read-only open of an empty journal is CX_ERR_FORMAT (nothing to
 * inspect). */
int osh_journal_open(struct osh_journal *j, const CxJournalOps *ops, void *ctx, int durable, const char *label, int ro);
void osh_journal_close(struct osh_journal *j);
/* 1 when an effect may be recorded: open, writable and not poisoned (the first failed append poisons it for good). */
int osh_journal_usable(const struct osh_journal *j);

/* Appends. Each returns CX_OK with *id, or the error; any failure poisons the journal. */
int oshj_put_intent(struct osh_journal *j, const struct oshj_effect *e, uint64_t *id);
int oshj_put_started(struct osh_journal *j, uint64_t intent, const struct oshj_effect *e, unsigned what, uint64_t *id);
int oshj_put_outcome(struct osh_journal *j, uint64_t intent, const struct oshj_effect *e, unsigned outcome, unsigned err,
                     int32_t status, int recovered, uint64_t *id);
/* The pipeline receipt: `outcomes` are the n outcome ids of the pipeline in the order they were written. */
int oshj_put_receipt(struct osh_journal *j, const struct oshj_effect *pl, const uint64_t *outcomes, unsigned n,
                     unsigned outcome, unsigned err, int32_t status, int recovered, uint64_t *id);
/* Operator reconciliation of an OUTCOME_UNKNOWN effect (the caller has checked the operator's authority). */
int oshj_put_reconcile(struct osh_journal *j, uint64_t intent, uint64_t unknown_outcome, unsigned verdict,
                       const uint8_t principal[32], uint8_t domain, uint32_t cap_index, uint64_t cap_generation,
                       uint16_t resource_class, uint16_t operation, uint64_t *id);

/* ---- read-only inspection ---- */
enum oshj_cat {
    OSHJ_CAT_FINISHED = 1,       /* an outcome written by the adapter after observed completion */
    OSHJ_CAT_UNKNOWN_INTENT = 2, /* intent only: the effect may or may not have begun */
    OSHJ_CAT_UNKNOWN_STARTED = 3,/* intent and STARTED: it began; its end is unknown */
    OSHJ_CAT_RECOVERED = 4,      /* OUTCOME_UNKNOWN written by an earlier recovery */
    OSHJ_CAT_RECONCILED = 5      /* recovered, then an operator's RECONCILE */
};
#define OSHJ_CATS 6u
struct oshj_item {
    uint64_t intent_id, started_id, outcome_id, reconcile_id;
    struct oshj_effect e;
    uint8_t cat, outcome, err, recovered, verdict, sealed; /* sealed: its pipeline has a receipt */
    int32_t status;
};
struct oshj_report {
    uint64_t records;            /* every Cortex object */
    uint32_t osh, other, bad;    /* OSH records; other writers' (facts); OSH kinds that do not decode or link nowhere */
    uint32_t intents, started, outcomes, receipts, reconciles, dup_outcomes;
    uint32_t cat[OSHJ_CATS];     /* intents per category */
    uint32_t pipelines, sealed, unsealed_finished; /* with an intent; with a receipt; every intent closed but no receipt */
    uint32_t max_epoch;
    int chain_ok;                /* cx_verify_chain */
};
/* Reads every record (no write, no allocation). each (optional) is called once per intent, in journal order. */
void oshj_inspect(const CxStore *cx, struct oshj_report *rep, void (*each)(void *u, const struct oshj_item *it), void *u);
/* One intent by id (0 if it is not an intent). */
int oshj_item(const CxStore *cx, uint64_t intent, struct oshj_item *it);
/* Names for the evidence lines. */
const char *oshj_cat_name(unsigned c);
const char *oshj_effect_name(unsigned e);

/* ---- recovery (never launches anything) ---- */
struct oshj_recovery {
    uint32_t unknown_intent, unknown_started; /* open intents found, by category */
    uint32_t outcomes_written, receipts_written, failed;
};
/* each (optional) is called once per open intent, after its OUTCOME_UNKNOWN was written (it->outcome_id) or failed. */
int oshj_recover(struct osh_journal *j, struct oshj_recovery *rv, void (*each)(void *u, const struct oshj_item *it),
                 void *u);

#ifdef CK_OSH_OP_TEST
#ifdef CK_HARDWARE_STAGING
#error "the RAM journal backend is test-only and refused under CK_HARDWARE_STAGING"
#endif
/* ---- TEST-ONLY RAM backend: the same Cortex bytes in a caller's buffer, NOT durable ----
 * Appends are all or nothing (like the sealed Store's). Fault injection: fail_at = the Nth append attempt (1-based,
 * counted over the backend's life) fails with CX_ERR_IO and writes nothing; unavailable = 1 refuses every open (the
 * journal device is absent). One writable handle at a time (CX_ERR_WRITER). */
struct oshj_ram {
    uint8_t *buf;
    size_t cap, len;
    int writer, ro_handles;
    uint32_t attempts, appends, fail_at;
    int unavailable;
};
void oshj_ram_init(struct oshj_ram *r, uint8_t *buf, size_t cap);
extern const CxJournalOps oshj_ram_ops;
#endif

#endif
