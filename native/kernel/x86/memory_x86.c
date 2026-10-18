/* memory_x86.c -- Allen's memory on the iMac's own hard disk.
 *
 * Approved place (Drake, 2026-10-17): a section of the internal 1 TB SATA
 * hard disk, the one whose IDENTIFY model is exactly "APPLE HDD ST1000DM003".
 * Any other disk, and every NVMe disk, is never written; with no such disk,
 * or two of them, Allen remembers only until the power goes off.
 *
 * At boot, once:
 *   1. every partition of that disk is listed on the console ("ahci_part:"),
 *      a record of the layout as found;
 *   2. if the GPT has no AIENOS partition, one of ALLEN_PART_MIB is added in
 *      unused space (dev/gpt_add.c): only then are the GPT blocks (LBA 1 up
 *      to the first usable LBA, and after the last usable LBA) opened for
 *      writing, and closed again right after. Existing partitions are never
 *      touched; no unused space, an invalid GPT or no GPT: nothing written;
 *   3. the write range is set to the two memory copies at the start of the
 *      AIENOS partition (svc/allen_memory.h) and nothing else, and the
 *      newest valid copy is loaded.
 * The conversation itself never goes on the console or the network report
 * from here: only counts ("3 facts", "name known"). */
#include "../core/ck_internal.h"
#include "../svc/allen_memory.h"
#include "disk_part.h"
#include "gpt_add.h"
#include "x86.h"

#define ALLEN_DISK_MODEL "APPLE HDD ST1000DM003"
#define ALLEN_PART_MIB 1024u

static ck_part part;
static ck_allen_store store;
static unsigned disk_index;
static int lasting;
static uint64_t hz;

static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void show_entry(void *ctx, const ck_gpt_entry *e)
{
    const disk_dev *d = ctx;
    const uint8_t *t = e->type;
    ck_printf("ahci_part: disk=%u entry=%u type=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x "
              "first_lba=%llu last_lba=%llu mib=%llu name=\"%s\"%s\n",
              disk_index, e->index, t[3], t[2], t[1], t[0], t[5], t[4], t[7], t[6], t[8], t[9], t[10], t[11], t[12],
              t[13], t[14], t[15], (unsigned long long)e->first, (unsigned long long)e->last,
              (unsigned long long)((e->last - e->first + 1u) * d->block_size >> 20), e->name,
              memcmp(t, ck_aienos_part_type, 16) ? "" : " (AIENOS)");
}

static int no_memory(const char *why)
{
    ck_printf("allen_memory: %s; Allen remembers only until this iMac turns off\n", why);
    return 0;
}

int ck_x86_memory_open(ck_allen_memory *mem, uint64_t tsc_hz)
{
    hz = tsc_hz;
    const disk_dev *d = 0;
    unsigned found = 0;
    for (unsigned i = 0;; i++) {
        const char *model = 0;
        const disk_dev *x = ck_x86_ahci_dev(i, &model);
        if (!x)
            break;
        if (same_text(model, ALLEN_DISK_MODEL)) {
            d = x;
            disk_index = i;
            found++;
        }
    }
    if (found != 1)
        return no_memory(found ? "two disks named " ALLEN_DISK_MODEL ", writing to neither"
                               : "no disk named " ALLEN_DISK_MODEL);
    ck_printf("allen_memory: the approved disk is ahci disk %u (" ALLEN_DISK_MODEL ", %llu blocks of %u bytes)\n",
              disk_index, (unsigned long long)d->block_count, d->block_size);
    int n = ck_gpt_list(d, show_entry, (void *)d);
    if (n < 0)
        return no_memory(ck_gpt_add_strerror(n)); /* no GPT or an invalid one: never written */
    ck_printf("allen_memory: the disk has %d partition%s\n", n, n == 1 ? "" : "s");

    ck_gpt_info gi;
    int rc = ck_gpt_find_aienos(d, &part, &gi);
    if (rc == CK_GPT_ENOPART) {
        uint64_t per_mib = (1024u * 1024u) / d->block_size;
        if (ck_x86_ahci_allow(disk_index, 1, gi.first_usable - 1u) ||
            ck_x86_ahci_allow(disk_index, gi.last_usable + 1u, gi.last_lba)) {
            ck_x86_ahci_allow_none(disk_index);
            return no_memory("could not open the GPT blocks for writing");
        }
        ck_gpt_added a;
        rc = ck_gpt_add_aienos(d, (uint64_t)ALLEN_PART_MIB * per_mib, per_mib, &a);
        ck_x86_ahci_allow_none(disk_index);
        if (rc) {
            ck_printf("allen_memory: AIENOS partition not added: %s%s\n", ck_gpt_add_strerror(rc),
                      rc == CK_GPT_EWRITE || rc == CK_GPT_EVERIFY ? " (the GPT may need a look)"
                                                                 : "; nothing was written");
            return no_memory("no AIENOS partition");
        }
        ck_printf("allen_memory: AIENOS partition added, entry %u, first_lba=%llu last_lba=%llu (%u MiB); the existing "
                  "partitions were not touched\n", a.index, (unsigned long long)a.first, (unsigned long long)a.last,
                  ALLEN_PART_MIB);
        rc = ck_gpt_find_aienos(d, &part, &gi);
    } else if (rc == CK_GPT_OK) {
        ck_printf("allen_memory: AIENOS partition found, entry %u, first_lba=%llu last_lba=%llu\n", gi.part_index,
                  (unsigned long long)part.first_lba, (unsigned long long)(part.first_lba + part.blocks - 1u));
    }
    if (rc)
        return no_memory(ck_gpt_add_strerror(rc));

    uint64_t copies = (uint64_t)CK_ALLEN_MEM_UNITS * CK_ALLEN_MEM_UNIT / d->block_size;
    if (part.blocks < copies || ck_x86_ahci_allow(disk_index, part.first_lba, part.first_lba + copies - 1u))
        return no_memory("could not open the memory copies for writing");
    rc = ck_allen_store_load(&store, &part.dev, mem);
    if (rc < 0) {
        ck_x86_ahci_allow_none(disk_index);
        return no_memory("reading the memory copies failed");
    }
    if (rc == CK_ALLEN_MEM_EMPTY)
        ck_puts("allen_memory: empty (a first start); Allen starts fresh\n");
    else
        ck_printf("allen_memory: loaded generation %llu (%u of 2 copies valid): %u fact%s, name %s, %u earlier start%s\n",
                  (unsigned long long)store.gen, store.valid, mem->nfacts, mem->nfacts == 1 ? "" : "s",
                  mem->name[0] ? "known" : "not told", mem->starts, mem->starts == 1 ? "" : "s");
    lasting = 1;
    return 1;
}

int ck_x86_memory_save(const ck_allen_memory *mem)
{
    if (!lasting)
        return -1;
    uint64_t t0 = ck_rdtsc();
    int rc = ck_allen_store_save(&store, mem);
    if (rc) {
        lasting = 0;
        ck_x86_ahci_allow_none(disk_index);
        ck_printf("allen_memory: save FAILED (rc=%d); nothing more is written this start\n", rc);
        return -1;
    }
    ck_printf("allen_memory: saved generation %llu to copy %c (%u fact%s) in %llu ms\n", (unsigned long long)store.gen,
              store.newest ? 'B' : 'A', mem->nfacts, mem->nfacts == 1 ? "" : "s",
              hz ? (unsigned long long)((ck_rdtsc() - t0) * 1000 / hz) : 0ull);
    return 0;
}
