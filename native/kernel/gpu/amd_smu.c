/* native/kernel/gpu/amd_smu.c -- SMU (System Management Unit) state check for
 * the 2017 iMac's AMD Radeon Pro 555/560 (Polaris 11).
 *
 * Apple's EFI leaves the SMC running with its full power-management firmware.
 * We check: is it alive (answers MSG_GetSMFWVersion), what version, and what
 * power state it reports.  We do NOT load or reset the SMC -- that would
 * require the polaris10 SMC firmware blob to be present and the full init
 * sequence (gfx_v8_0 + powerplay).  Those are Steps 4-6.
 *
 * If the SMC is alive, we can use it later for fan and power-limit control.
 * If it is not alive, we still proceed -- CP / SDMA do not need the SMC, and
 * display works without it.
 *
 * Sources: include/asic_reg/smu/smu_7_1_3_d.h MIT (C) 2016 AMD,
 *          pm/powerplay/smumgr/polaris10_smumgr.c MIT (C) 2016 AMD.
 * No floats.  All waits TSC-bounded. */
#include "amd_gpu.h"

/* SMC indirect access helper (reads 32 bits from SMC address space). */
static uint32_t smc_ind_rd(uint32_t addr)
{
    amd_wr(mmSMC_IND_INDEX_0, addr);
    __asm__ volatile("" ::: "memory");
    return amd_rd(mmSMC_IND_DATA_0);
}

/* Send a SMC message and wait for the response (polaris10_smumgr.c).
 * Returns AMD_RING_OK or AMD_RING_ETIMEOUT on no response.
 * The response value is written to *resp (ignored on timeout). */
static int smu_send_msg(uint32_t msg, uint32_t arg, uint32_t *resp)
{
    /* Clear previous response */
    amd_wr(mmSMC_MSG_RESP_0, 0);
    __asm__ volatile("" ::: "memory");

    /* Write argument then message */
    amd_wr(mmSMC_MSG_ARG_0, arg);
    amd_wr(mmSMC_MSG, msg);

    /* Poll for a non-zero response (bit 0 = DONE in some Polaris fw builds;
     * any non-zero value means the SMC replied). */
    uint64_t t0    = ck_rdtsc();
    uint64_t ticks = g_amd_gpu.tsc_hz / 1000ull * GPU_SMU_MSG_MS;
    uint32_t r;
    do {
        r = amd_rd(mmSMC_MSG_RESP_0);
        if (r != 0) break;
        ck_pause();
    } while (ck_rdtsc() - t0 < ticks);

    if (r == 0) return AMD_RING_ETIMEOUT;
    if (resp) *resp = r;
    return AMD_RING_OK;
}

int amd_smu_check(void)
{
    struct amd_gpu_state *g = &g_amd_gpu;

    if (!g->regs) {
        ck_puts("gpu: smu: no register aperture\n");
        return -1;
    }

    /* Read SMC message registers directly to see if they look sane. */
    uint32_t msg_reg  = amd_rd(mmSMC_MSG);
    uint32_t resp_reg = amd_rd(mmSMC_MSG_RESP_0);
    ck_printf("gpu: smu: SMC_MSG=0x%08x  SMC_MSG_RESP=0x%08x\n", msg_reg, resp_reg);

    if (msg_reg == 0xffffffffu && resp_reg == 0xffffffffu) {
        ck_puts("gpu: smu: registers read all-ones (SMU not accessible)\n");
        g->smu_alive = 0;
        g->steps_done |= AMD_STEP_SMU;
        return 0;  /* non-fatal: proceed without SMU */
    }

    /* Try to read the SMC firmware version from the firmware header in VRAM.
     * Apple's firmware loads the SMC blob; the version is at a fixed offset
     * in the SMC header (polaris10_smumgr.c SMU7_FIRMWARE_HEADER_LOCATION). */
    uint32_t fw_ver_indirect = 0;
    if (g->vram_cpu) {
        /* Read from VRAM directly (VRAM mapped via BAR0). */
        volatile uint32_t *hdr = (volatile uint32_t *)
            ((volatile uint8_t *)g->vram_cpu + SMU7_FIRMWARE_HEADER_LOCATION +
             SMU7_FIRMWARE_HEADER_smc_firmware_version_offset);
        fw_ver_indirect = *hdr;
        ck_printf("gpu: smu: SMC header at VRAM+0x%x: fw_version=0x%08x\n",
                  SMU7_FIRMWARE_HEADER_LOCATION + SMU7_FIRMWARE_HEADER_smc_firmware_version_offset,
                  fw_ver_indirect);
    } else {
        /* Try SMC indirect access */
        fw_ver_indirect = smc_ind_rd(SMU7_FIRMWARE_HEADER_LOCATION +
                                     SMU7_FIRMWARE_HEADER_smc_firmware_version_offset);
        ck_printf("gpu: smu: SMC indirect fw_version=0x%08x\n", fw_ver_indirect);
    }

    /* Ping the SMC with MSG_GetSMFWVersion.  Polaris10 firmware responds with
     * the version in the response register.  PPSMC_MSG_GetSMFWVersion = 0x0201. */
    uint32_t resp = 0;
    int rc = smu_send_msg(PPSMC_MSG_GetSMFWVersion, 0, &resp);
    if (rc != AMD_RING_OK) {
        ck_printf("gpu: smu: no response to MSG_GetSMFWVersion within %u ms; "
                  "SMC not running or not Polaris10 firmware\n", GPU_SMU_MSG_MS);
        g->smu_alive    = 0;
        g->smu_fw_ver   = fw_ver_indirect;
    } else {
        g->smu_alive  = 1;
        g->smu_fw_ver = resp;
        ck_printf("gpu: smu: SMC alive, firmware version 0x%08x (from MSG_GetSMFWVersion)\n",
                  g->smu_fw_ver);
    }

    /* Decision: do we need to load SMC firmware?
     * Rule (from polaris10_smumgr.c): if the SMC is alive and the version
     * matches the polaris11 blob version we ship, skip the load.  If it is
     * not alive (or a different version), load it in Step 4.
     * For now: report what we found; the ucode step handles the decision. */
    ck_printf("gpu: smu: smu_alive=%d fw_ver=0x%08x (header=0x%08x)\n",
              g->smu_alive, g->smu_fw_ver, fw_ver_indirect);

    g->steps_done |= AMD_STEP_SMU;
    ck_puts("gpu: smu: done\n");
    return 0;
}
