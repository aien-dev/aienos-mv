/* rng_bind.h -- devices-stage bind of a contract-generated virtio entropy
 * driver (Allen's driver builder, L268; contract model ADR 0021).
 *
 * Compiled only into an image built with CK_CONTRACT_RNG=<staged dir>, where
 * <staged dir>/driver holds the generated vrng driver (class entropy-poll,
 * prefix vrng) of the one contract the user consented to. The bind:
 *   1. finds the first PCI function whose vendor:device is in the contract's
 *      own match table (vrng_match_ids), never any other function;
 *   2. refuses an UNPROVEN contract unless the image is an attended image
 *      (CK_CONTRACT_RNG_ATTENDED=1, written only after a consent record);
 *   3. confines the one DMA region the contract declares behind the SMMU
 *      before bus master is turned on (no SMMU: denied, nothing touched);
 *   4. maps only the capability windows the contract names, inside their BAR;
 *   5. runs the generated bring-up with VIRTIO_F_ACCESS_PLATFORM required,
 *      reads entropy, prints it, then resets the device, turns bus master off
 *      and returns the stream to abort. The vendor driver (when there is one)
 *      is never touched: this image simply does not carry it.
 * QEMU is not hardware: a pass here qualifies nothing physical. */
#ifndef AIENOS_CK_RNG_BIND_H
#define AIENOS_CK_RNG_BIND_H
#include "pci.h"

enum {
    CK_RNG_OK = 0,
    CK_RNG_ABSENT = 1,       /* no function matches the contract */
    CK_RNG_E_UNPROVEN = -1,  /* contract not proven and the image is not attended */
    CK_RNG_E_CAPS = -2,
    CK_RNG_E_DMA = -3,
    CK_RNG_E_DENIED = -4,    /* SMMU confinement not available */
    CK_RNG_E_INIT = -5,
    CK_RNG_E_READ = -6,
};

/* Bind, read, release. Returns a CK_RNG_* code; *bound_out (may be NULL)
 * receives the function it bound, for the inventory. */
int ck_rng_bind_selftest(const pci_system *s, const pci_func **bound_out);
int ck_rng_live(void);
void ck_rng_release(void);
/* The contract's 12-hex-digit digest prefix and proof state, for the inventory. */
const char *ck_rng_contract_short(void);
const char *ck_rng_contract_proven(void);

#endif
