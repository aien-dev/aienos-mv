//! Fuzzer backend: a random walk over the contract's state machine
//! (bring-up outcomes, data path, hostile completions, reset) with the
//! driver under test running against the generated chip model. It checks
//! what the contract promises: the driver never touches an undeclared
//! register or breaks a protocol rule (model violations stay 0), every
//! descriptor stays inside the DMA region, the bring-up result is the one the
//! contract predicts for the device's offer, a hostile completion makes the
//! device broken and broken is sticky with no further device access.

use crate::contract::{Contract, Policy};
use crate::driver::banner;
use std::fmt::Write as _;

pub fn source(c: &Contract) -> String {
    let p = &c.prefix;
    let up = c.up();
    let mut o = banner(c, "/*", "*/");
    // predicted init result from the offer, in the contract's negotiate order
    let mut predict = String::new();
    for f in c.features.iter().filter(|f| f.policy == Policy::Require) {
        let _ = writeln!(
            predict,
            "    if (!(offer >> {} & 1)) return {up}_E_{};",
            f.bit,
            f.else_err.as_ref().unwrap()
        );
    }
    for f in c.features.iter().filter(|f| f.required_by.is_some()) {
        let _ = writeln!(
            predict,
            "    if ((flags & {up}_INIT_{}) && !(offer >> {} & 1)) return {up}_E_{};",
            f.required_by.as_ref().unwrap(),
            f.bit,
            f.else_err.as_ref().unwrap()
        );
    }
    let flags_all = c.flags.iter().map(|(_, v)| *v).fold(0, |a, v| a | v);
    let feat_bits: Vec<u32> = c.features.iter().map(|f| f.bit).collect();
    let _ = write!(
        o,
        r#"/* Usage: {p}_fuzz [seed] [walks]. Prints {UP}_FUZZ: PASS|FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aienos_virtio_net.h"
#include "vnet_model.h"

static uint64_t rng;
static uint32_t rnd(void) {{ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }}
static unsigned fails;
static unsigned long checks, visits[8];
static const char *const state_name[8] = {{"init-refused", "live", "tx", "rx", "reclaim", "hostile", "broken", "reset"}};
#define CHECK(c, ...) do {{ checks++; if (!(c)) {{ if (fails++ < 20) {{ fprintf(stderr, "FAIL walk %lu: ", walk); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); }} }} }} while (0)

static uint8_t region[1 << 20] __attribute__((aligned(4096)));
static const uint64_t DMA = 0x40000000ull;
static unsigned long walk;

static {p}_err predicted(uint64_t offer, uint32_t flags)
{{
{predict}    return {up}_OK;
}}

static void descs_inside(const {p}_dev *d)
{{
    for (unsigned q = 0; q < 2; q++)
        for (unsigned i = 0; i < d->q[q].size; i++) {{
            uint64_t a; uint32_t l;
            memcpy(&a, d->mem + d->q[q].desc_off + 16u * i, 8);
            memcpy(&l, d->mem + d->q[q].desc_off + 16u * i + 8, 4);
            if (!a && !l) continue;
            CHECK(a >= d->dma && a - d->dma + l <= d->mem_len, "descriptor %u/%u outside the region", q, i);
        }}
}}

int main(int argc, char **argv)
{{
    rng = argc > 1 ? strtoull(argv[1], 0, 0) : 0x6a5e11;
    if (!rng) rng = 1;
    unsigned long walks = argc > 2 ? strtoul(argv[2], 0, 0) : 2000;
    static const unsigned feat_bits[] = {{{feat_bits}}};
    for (walk = 0; walk < walks; walk++) {{
        static {p}_model m;
        memset(&m, 0, sizeof m);
        /* a random device: each declared feature offered with p=7/8, plus noise bits */
        uint64_t offer = (uint64_t)rnd() << 32 | rnd();
        for (unsigned k = 0; k < sizeof feat_bits / sizeof feat_bits[0]; k++) {{
            offer &= ~(1ull << feat_bits[k]);
            if (rnd() % 8) offer |= 1ull << feat_bits[k];
        }}
        m.refuse_features = rnd() % 16 == 0;
        m.dev_features = offer;
        m.numq = 2;
        m.mult = 1u << (rnd() % 4);
        uint16_t qsize = (uint16_t)(1u << (1 + rnd() % 8));
        for (unsigned q = 0; q < 2; q++) m.qmax[q] = (uint16_t)(1u << (1 + rnd() % 9));
        for (unsigned i = 0; i < 6; i++) m.config[i] = (uint8_t)rnd();
        uint32_t flags = rnd() % 2 ? {flags_all}u : 0;
        size_t need = {p}_mem_size(qsize);
        {p}_model_init(&m, region, DMA, need);
        {p}_ops ops = {p}_model_ops(&m);
        virtio_pci_caps caps;
        memset(&caps, 0, sizeof caps);
        caps.common_cfg = (virtio_pci_region){{1, 4, 0, 0x1000}};
        caps.isr_cfg = (virtio_pci_region){{1, 4, 0x1000, 0x1000}};
        caps.device_cfg = (virtio_pci_region){{1, 4, 0x2000, 0x1000}};
        caps.notify_cfg = (virtio_pci_region){{1, 4, 0x3000, 0x1000}};
        caps.has_notify_off_multiplier = 1;
        caps.notify_off_multiplier = m.mult;
        {p}_dev d;
        {p}_err e = {p}_init_flags(&d, &ops, &caps, region, DMA, need, qsize, flags);
        {p}_err want = predicted(offer, flags);
        unsigned smaller = m.qmax[0] < {UP}_QUEUE_MIN || m.qmax[1] < {UP}_QUEUE_MIN;
        if (want == {up}_OK && m.refuse_features) want = {up}_E_FEATURES;
        if (want == {up}_OK && smaller) want = {up}_E_QUEUES;
        CHECK(e == want, "init %s, contract predicts %s (offer %016llx flags %u)", {p}_err_name(e), {p}_err_name(want), (unsigned long long)offer, flags);
        CHECK(m.violations == 0, "init: %s", m.last_violation);
        if (e != {up}_OK) {{
            visits[0]++;
            CHECK(d.broken, "refused init left the device usable");
            CHECK(({p}_model_status(&m) & {UP}_S_FAILED) || e == {up}_E_RESET_TIMEOUT, "refused init did not mark FAILED");
            continue;
        }}
        visits[1]++;
        CHECK(d.features == ((offer & d.features) | (1ull << {UP}_F_VERSION_1)), "accepted a feature that was not offered");
        CHECK(d.q[0].size <= m.qmax[0] && d.q[1].size <= m.qmax[1], "queue size above the device maximum");
        /* poisoned: a hostile completion is planted and not yet consumed. The
         * walk may poke the other queue first, so the driver may report it on
         * any later rx/tx/reclaim; until then rx contents are not judged. */
        int broken = 0, poisoned = 0;
        for (unsigned stepn = 0; stepn < 64; stepn++) {{
            uint8_t f[{UP}_FRAME_MAX], out[{UP}_FRAME_MAX];
            size_t n = 14 + rnd() % ({UP}_FRAME_MAX - 13), got_len = 0;
            int got = 0;
            unsigned before = m.accesses;
            e = {up}_OK;
            switch (rnd() % 8) {{
            case 0: case 1:
                for (size_t i = 0; i < n; i++) f[i] = (uint8_t)rnd();
                e = {p}_tx(&d, f, n);
                visits[2]++;
                CHECK(e == {up}_OK || e == {up}_E_FULL || (broken && e == {up}_E_BROKEN) || (poisoned && e == {up}_E_DEVICE), "tx: %s", {p}_err_name(e));
                break;
            case 2:
                {p}_model_tx(&m);
                e = {p}_tx_reclaim(&d, 0);
                visits[4]++;
                CHECK(e == {up}_OK || (broken && e == {up}_E_BROKEN) || (poisoned && e == {up}_E_DEVICE), "reclaim: %s", {p}_err_name(e));
                break;
            case 3: case 4:
                for (size_t i = 0; i < n; i++) f[i] = (uint8_t)rnd();
                int delivered = {p}_model_rx(&m, f, n);
                e = {p}_rx(&d, out, sizeof out, &got_len, &got);
                visits[3]++;
                if (broken) CHECK(e == {up}_E_BROKEN, "rx after broken: %s", {p}_err_name(e));
                else if (poisoned) CHECK(e == {up}_OK || e == {up}_E_DEVICE, "rx with a planted completion: %s", {p}_err_name(e));
                else if (delivered) CHECK(e == {up}_OK && got && got_len == n && !memcmp(out, f, n), "rx lost or changed a frame");
                break;
            case 5:
                if (rnd() % 4) break;
                visits[5]++;
                switch (rnd() % 3) {{
                case 0: {p}_model_push_used(&m, rnd() % 2, d.q[0].size + rnd() % 64, 0); break;
                case 1: {p}_model_push_used(&m, 0, 0, {UP}_BUF_LEN + 1 + rnd() % 64); break;
                default: {p}_model_bump_used(&m, rnd() % 2, (uint16_t)(1000 + rnd() % 1000)); break;
                }}
                poisoned = 1;
                e = rnd() % 2 ? {p}_rx(&d, out, sizeof out, &got_len, &got) : {p}_tx_reclaim(&d, 0);
                CHECK(e == {up}_E_DEVICE || e == {up}_E_BROKEN || e == {up}_OK, "hostile: %s", {p}_err_name(e));
                if (e == {up}_E_DEVICE) {{ CHECK(d.broken, "hostile completion did not break the device"); broken = 1; }}
                break;
            case 6:
                if (!broken) break;
                visits[6]++;
                e = {p}_tx(&d, f, 60);
                CHECK(e == {up}_E_BROKEN && m.accesses == before, "broken device still driven (%s, %u accesses)", {p}_err_name(e), m.accesses - before);
                break;
            default:
                if (rnd() % 8) break;
                visits[7]++;
                e = {p}_init_flags(&d, &ops, &caps, region, DMA, need, qsize, flags);
                CHECK(e == {up}_OK, "re-init after reset: %s", {p}_err_name(e));
                broken = 0;
                poisoned = 0;
                break;
            }}
            if (e == {up}_E_DEVICE) {{
                CHECK(poisoned, "HostileDevice with no hostile completion planted");
                CHECK(d.broken, "hostile completion did not break the device");
                broken = 1;
                poisoned = 0;
            }}
            CHECK(m.violations == 0, "step %u: %s", stepn, m.last_violation);
            if (!broken) descs_inside(&d);
            if (m.violations) break;
        }}
    }}
    printf("{p}_fuzz: seed walks=%lu checks=%lu", walks, checks);
    for (int s = 0; s < 8; s++) printf(" %s=%lu", state_name[s], visits[s]);
    printf("\n");
    for (int s = 0; s < 8; s++) if (!visits[s]) {{ fails++; fprintf(stderr, "FAIL state %s never visited\n", state_name[s]); }}
    printf("{UP}_FUZZ: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}}
"#,
        UP = up,
        feat_bits = feat_bits
            .iter()
            .map(|b| b.to_string())
            .collect::<Vec<_>>()
            .join(", "),
    );
    o
}
