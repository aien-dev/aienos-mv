/* ck_pi5_rc_guard.h -- compile-time fence of the Pi 5 release candidate (make pi5-rc, CK_PI5_RC=1).
 *
 * Force-included (gcc -include) into every C and assembly unit of the RC image. GCC processes -include after
 * every -D/-U on the command line and before the first line of the source, so this sees exactly the macros the
 * build passed in, and nothing a source file defines later. Each test, mutant, unsafe-DMA, fw_cfg, probe,
 * self-test or hardware-staging switch the sources know is refused here, whatever route it took into the flags.
 *
 * The list below is generated from the sources (every CK_/CC_/CM_/CR_/CS_/RC_ name containing MUTANT, MUTATION,
 * TEST, UNSAFE, BYPASS, SELFTEST, STAGING or PROBE, except CK_INFER_PROBE, which is the lifecycle's own
 * boot-disk model reader) plus six switches named by hand (CK_CONSOLE_SESSION, CK_ART_STORE_WRITER,
 * CK_SLOT_UPDATE_KEYS, CK_ARTIFACT_CARD, CK_PI5_STORE, CK_PI5_PRODUCTION), one sorted list. scripts/ck_pi5_rc_check.sh
 * fails when a source gains such a name that is missing here, and proves a sample of these lines fire.
 * Preprocessor lines only: the assembly units include it too. */
#ifndef AIENOS_CK_PI5_RC_GUARD_H
#define AIENOS_CK_PI5_RC_GUARD_H

#if !defined(CK_PI5_RC) || !defined(CK_PI5) || !defined(CK_PI5_UNLOCK) || !defined(CK_PI5_UNLOCK_CARD) || \
    !defined(CK_PI5_LIFECYCLE)
#error "ck_pi5_rc_guard.h: the Pi 5 release candidate is the CK_PI5 unlock-card lifecycle image with CK_PI5_RC=1"
#endif

/* refused switches */
#ifdef CC_MUTANT_ACCEPT_RESERVED
#error "Pi 5 release candidate refuses CC_MUTANT_ACCEPT_RESERVED"
#endif
#ifdef CC_MUTANT_CHILD_INDEX_LE
#error "Pi 5 release candidate refuses CC_MUTANT_CHILD_INDEX_LE"
#endif
#ifdef CC_MUTANT_MANIFEST_LAST_WAL_ZERO
#error "Pi 5 release candidate refuses CC_MUTANT_MANIFEST_LAST_WAL_ZERO"
#endif
#ifdef CC_MUTANT_SKIP_D1
#error "Pi 5 release candidate refuses CC_MUTANT_SKIP_D1"
#endif
#ifdef CC_MUTANT_SKIP_D2
#error "Pi 5 release candidate refuses CC_MUTANT_SKIP_D2"
#endif
#ifdef CC_MUTANT_SKIP_TAKE_BOUND
#error "Pi 5 release candidate refuses CC_MUTANT_SKIP_TAKE_BOUND"
#endif
#ifdef CC_MUTANT_TRUNCATE_AT_CAP
#error "Pi 5 release candidate refuses CC_MUTANT_TRUNCATE_AT_CAP"
#endif
#ifdef CC_MUTANT_UNCHECKED_FORK_SUM
#error "Pi 5 release candidate refuses CC_MUTANT_UNCHECKED_FORK_SUM"
#endif
#ifdef CK_ACARD_MUTANT_TRAILING
#error "Pi 5 release candidate refuses CK_ACARD_MUTANT_TRAILING"
#endif
#ifdef CK_AGENT_CMD_MUTANT_ANY_VERB
#error "Pi 5 release candidate refuses CK_AGENT_CMD_MUTANT_ANY_VERB"
#endif
#ifdef CK_AGENT_MUTANT_MODEL_FIRST
#error "Pi 5 release candidate refuses CK_AGENT_MUTANT_MODEL_FIRST"
#endif
#ifdef CK_AGENT_MUTANT_NO_COMMIT
#error "Pi 5 release candidate refuses CK_AGENT_MUTANT_NO_COMMIT"
#endif
#ifdef CK_AGENT_MUTANT_NO_GOLDEN_CHECK
#error "Pi 5 release candidate refuses CK_AGENT_MUTANT_NO_GOLDEN_CHECK"
#endif
#ifdef CK_AGENT_MUTANT_NO_PROMPT_CHECK
#error "Pi 5 release candidate refuses CK_AGENT_MUTANT_NO_PROMPT_CHECK"
#endif
#ifdef CK_AGENT_MUTANT_RECOVER_BLIND
#error "Pi 5 release candidate refuses CK_AGENT_MUTANT_RECOVER_BLIND"
#endif
#ifdef CK_AGENT_MUTANT_RECOVER_NO_CHECK
#error "Pi 5 release candidate refuses CK_AGENT_MUTANT_RECOVER_NO_CHECK"
#endif
#ifdef CK_AML_MUTANT
#error "Pi 5 release candidate refuses CK_AML_MUTANT"
#endif
#ifdef CK_ARTIFACT_CARD
#error "Pi 5 release candidate refuses CK_ARTIFACT_CARD"
#endif
#ifdef CK_ART_STORE_WRITER
#error "Pi 5 release candidate refuses CK_ART_STORE_WRITER"
#endif
#ifdef CK_B7B_PROBE
#error "Pi 5 release candidate refuses CK_B7B_PROBE"
#endif
#ifdef CK_B7B_PROBE_TEST_SEGMENT0
#error "Pi 5 release candidate refuses CK_B7B_PROBE_TEST_SEGMENT0"
#endif
#ifdef CK_CONSOLE_SESSION
#error "Pi 5 release candidate refuses CK_CONSOLE_SESSION"
#endif
#ifdef CK_CONT_SERVICE_HOST_TEST
#error "Pi 5 release candidate refuses CK_CONT_SERVICE_HOST_TEST"
#endif
#ifdef CK_CTX_HOST_TEST
#error "Pi 5 release candidate refuses CK_CTX_HOST_TEST"
#endif
#ifdef CK_CTX_MUTANT_SKIP_BINDING
#error "Pi 5 release candidate refuses CK_CTX_MUTANT_SKIP_BINDING"
#endif
#ifdef CK_DMAW_MUTANT_
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_"
#endif
#ifdef CK_DMAW_MUTANT_NO_ALIGN
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_ALIGN"
#endif
#ifdef CK_DMAW_MUTANT_NO_END
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_END"
#endif
#ifdef CK_DMAW_MUTANT_NO_FRAMECAP
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_FRAMECAP"
#endif
#ifdef CK_DMAW_MUTANT_NO_GOT
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_GOT"
#endif
#ifdef CK_DMAW_MUTANT_NO_INDEX
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_INDEX"
#endif
#ifdef CK_DMAW_MUTANT_NO_LATCH
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_LATCH"
#endif
#ifdef CK_DMAW_MUTANT_NO_STALE
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_STALE"
#endif
#ifdef CK_DMAW_MUTANT_NO_ZERO
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_NO_ZERO"
#endif
#ifdef CK_DMAW_MUTANT_WRAP
#error "Pi 5 release candidate refuses CK_DMAW_MUTANT_WRAP"
#endif
#ifdef CK_FACT_MUTANT_NO_APPEND
#error "Pi 5 release candidate refuses CK_FACT_MUTANT_NO_APPEND"
#endif
#ifdef CK_FACT_MUTANT_RECALL_BLIND
#error "Pi 5 release candidate refuses CK_FACT_MUTANT_RECALL_BLIND"
#endif
#ifdef CK_FPU_PROBE
#error "Pi 5 release candidate refuses CK_FPU_PROBE"
#endif
#ifdef CK_GEM_MUTANT_
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_"
#endif
#ifdef CK_GEM_MUTANT_BME_EARLY
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_BME_EARLY"
#endif
#ifdef CK_GEM_MUTANT_BME_LEFT_ON
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_BME_LEFT_ON"
#endif
#ifdef CK_GEM_MUTANT_MDIO_OP
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_MDIO_OP"
#endif
#ifdef CK_GEM_MUTANT_NO_ADDR64
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_ADDR64"
#endif
#ifdef CK_GEM_MUTANT_NO_BARRIER
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_BARRIER"
#endif
#ifdef CK_GEM_MUTANT_NO_BME_VERIFY
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_BME_VERIFY"
#endif
#ifdef CK_GEM_MUTANT_NO_DIFF
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_DIFF"
#endif
#ifdef CK_GEM_MUTANT_NO_DRFCS
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_DRFCS"
#endif
#ifdef CK_GEM_MUTANT_NO_FRAMECAP
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_FRAMECAP"
#endif
#ifdef CK_GEM_MUTANT_NO_GBE
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_GBE"
#endif
#ifdef CK_GEM_MUTANT_NO_HWWIN
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_HWWIN"
#endif
#ifdef CK_GEM_MUTANT_NO_LLB_CLEAR
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_LLB_CLEAR"
#endif
#ifdef CK_GEM_MUTANT_NO_QUIESCE_WAIT
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_QUIESCE_WAIT"
#endif
#ifdef CK_GEM_MUTANT_NO_RETE_OFF
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_RETE_OFF"
#endif
#ifdef CK_GEM_MUTANT_NO_RXLEN_CHECK
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_RXLEN_CHECK"
#endif
#ifdef CK_GEM_MUTANT_NO_SENTINEL_USED
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_SENTINEL_USED"
#endif
#ifdef CK_GEM_MUTANT_NO_SEQ
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_SEQ"
#endif
#ifdef CK_GEM_MUTANT_NO_TIEOFF
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_TIEOFF"
#endif
#ifdef CK_GEM_MUTANT_NOT_STICKY
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NOT_STICKY"
#endif
#ifdef CK_GEM_MUTANT_NO_TX_LAST
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_TX_LAST"
#endif
#ifdef CK_GEM_MUTANT_NO_VALIDATE
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_VALIDATE"
#endif
#ifdef CK_GEM_MUTANT_NO_WRAP
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_NO_WRAP"
#endif
#ifdef CK_GEM_MUTANT_PHYLB_BIT
#error "Pi 5 release candidate refuses CK_GEM_MUTANT_PHYLB_BIT"
#endif
#ifdef CK_GSP_BINDATA_EXTRACT_SELFTEST
#error "Pi 5 release candidate refuses CK_GSP_BINDATA_EXTRACT_SELFTEST"
#endif
#ifdef CK_GSP_BOOT_ARGS_ENCODE_SELFTEST
#error "Pi 5 release candidate refuses CK_GSP_BOOT_ARGS_ENCODE_SELFTEST"
#endif
#ifdef CK_GSP_COT_ENCODE_SELFTEST
#error "Pi 5 release candidate refuses CK_GSP_COT_ENCODE_SELFTEST"
#endif
#ifdef CK_GSP_FW_INSPECT_SELFTEST
#error "Pi 5 release candidate refuses CK_GSP_FW_INSPECT_SELFTEST"
#endif
#ifdef CK_GSP_RPC_ENCODE_SELFTEST
#error "Pi 5 release candidate refuses CK_GSP_RPC_ENCODE_SELFTEST"
#endif
#ifdef CK_HANDOFF_MUTANT_SKIP_RESERVED
#error "Pi 5 release candidate refuses CK_HANDOFF_MUTANT_SKIP_RESERVED"
#endif
#ifdef CK_HARDWARE_STAGING
#error "Pi 5 release candidate refuses CK_HARDWARE_STAGING"
#endif
#ifdef CK_HOST_TEST
#error "Pi 5 release candidate refuses CK_HOST_TEST"
#endif
#ifdef CK_IORT_MUTANT_
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_"
#endif
#ifdef CK_IORT_MUTANT_OTHER_BLIND
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_OTHER_BLIND"
#endif
#ifdef CK_IORT_MUTANT_OVERLAP_OK
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_OVERLAP_OK"
#endif
#ifdef CK_IORT_MUTANT_RMR_BLIND
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_RMR_BLIND"
#endif
#ifdef CK_IORT_MUTANT_RMR_EXCLUSIVE
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_RMR_EXCLUSIVE"
#endif
#ifdef CK_IORT_MUTANT_ROUTE_FIRST
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_ROUTE_FIRST"
#endif
#ifdef CK_IORT_MUTANT_SEGMENT_BLIND
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_SEGMENT_BLIND"
#endif
#ifdef CK_IORT_MUTANT_TRUNCATE
#error "Pi 5 release candidate refuses CK_IORT_MUTANT_TRUNCATE"
#endif
#ifdef CK_LAYOUT_PROBE_UNITS
#error "Pi 5 release candidate refuses CK_LAYOUT_PROBE_UNITS"
#endif
#ifdef CK_NET_MUTANT_
#error "Pi 5 release candidate refuses CK_NET_MUTANT_"
#endif
#ifdef CK_NET_MUTANT_BAD_CLASS
#error "Pi 5 release candidate refuses CK_NET_MUTANT_BAD_CLASS"
#endif
#ifdef CK_NET_MUTANT_NO_ASSERT
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_ASSERT"
#endif
#ifdef CK_NET_MUTANT_NO_BOUNDS
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_BOUNDS"
#endif
#ifdef CK_NET_MUTANT_NO_CLK_VERIFY
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_CLK_VERIFY"
#endif
#ifdef CK_NET_MUTANT_NO_DISABLE_FIRST
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_DISABLE_FIRST"
#endif
#ifdef CK_NET_MUTANT_NO_EXACT_DIV
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_EXACT_DIV"
#endif
#ifdef CK_NET_MUTANT_NO_HIGH_FIRST
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_HIGH_FIRST"
#endif
#ifdef CK_NET_MUTANT_NO_HOLD
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_HOLD"
#endif
#ifdef CK_NET_MUTANT_NO_IDLE_AFTER
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_IDLE_AFTER"
#endif
#ifdef CK_NET_MUTANT_NO_MDC_DIV
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_MDC_DIV"
#endif
#ifdef CK_NET_MUTANT_NO_MPE
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_MPE"
#endif
#ifdef CK_NET_MUTANT_NO_OTHERS
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_OTHERS"
#endif
#ifdef CK_NET_MUTANT_NO_QUIESCE_CHECK
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_QUIESCE_CHECK"
#endif
#ifdef CK_NET_MUTANT_NO_READBACK
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_READBACK"
#endif
#ifdef CK_NET_MUTANT_NO_RELEASE
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_RELEASE"
#endif
#ifdef CK_NET_MUTANT_NO_REMAP_EN
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_REMAP_EN"
#endif
#ifdef CK_NET_MUTANT_NO_RESTORE
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_RESTORE"
#endif
#ifdef CK_NET_MUTANT_NO_STATUS_MASK
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_STATUS_MASK"
#endif
#ifdef CK_NET_MUTANT_NO_STRADDLE
#error "Pi 5 release candidate refuses CK_NET_MUTANT_NO_STRADDLE"
#endif
#ifdef CK_NET_MUTANT_OB_NO_LIMIT
#error "Pi 5 release candidate refuses CK_NET_MUTANT_OB_NO_LIMIT"
#endif
#ifdef CK_NET_MUTANT_OUI_ORDER
#error "Pi 5 release candidate refuses CK_NET_MUTANT_OUI_ORDER"
#endif
#ifdef CK_NET_MUTANT_PARTIAL_ANY
#error "Pi 5 release candidate refuses CK_NET_MUTANT_PARTIAL_ANY"
#endif
#ifdef CK_NET_MUTANT_TOUCH_ON_CLOCK
#error "Pi 5 release candidate refuses CK_NET_MUTANT_TOUCH_ON_CLOCK"
#endif
#ifdef CK_NVME_DMA_BYPASS
#error "Pi 5 release candidate refuses CK_NVME_DMA_BYPASS"
#endif
#ifdef CK_NVME_UNSAFE_BYPASS
#error "Pi 5 release candidate refuses CK_NVME_UNSAFE_BYPASS"
#endif
#ifdef CK_OSC_LAUNCH_TEST
#error "Pi 5 release candidate refuses CK_OSC_LAUNCH_TEST"
#endif
#ifdef CK_OSH_OP_MUTANT_SKIP_PERM
#error "Pi 5 release candidate refuses CK_OSH_OP_MUTANT_SKIP_PERM"
#endif
#ifdef CK_OSH_OP_TEST
#error "Pi 5 release candidate refuses CK_OSH_OP_TEST"
#endif
#ifdef CK_OSH_TEST
#error "Pi 5 release candidate refuses CK_OSH_TEST"
#endif
#ifdef CK_PCIE_MUTANT_
#error "Pi 5 release candidate refuses CK_PCIE_MUTANT_"
#endif
#ifdef CK_PCIE_MUTANT_NO_ALIGN
#error "Pi 5 release candidate refuses CK_PCIE_MUTANT_NO_ALIGN"
#endif
#ifdef CK_PCIE_MUTANT_NO_FULLCOVER
#error "Pi 5 release candidate refuses CK_PCIE_MUTANT_NO_FULLCOVER"
#endif
#ifdef CK_PCIE_MUTANT_NO_INDEX
#error "Pi 5 release candidate refuses CK_PCIE_MUTANT_NO_INDEX"
#endif
#ifdef CK_PCIE_MUTANT_NO_MERGE
#error "Pi 5 release candidate refuses CK_PCIE_MUTANT_NO_MERGE"
#endif
#ifdef CK_PCIE_MUTANT_NO_OUTSIDE
#error "Pi 5 release candidate refuses CK_PCIE_MUTANT_NO_OUTSIDE"
#endif
#ifdef CK_PCIE_MUTANT_NO_REMAP
#error "Pi 5 release candidate refuses CK_PCIE_MUTANT_NO_REMAP"
#endif
#ifdef CK_PI5_PRODUCTION
#error "Pi 5 release candidate refuses CK_PI5_PRODUCTION"
#endif
#ifdef CK_PI5_STORE
#error "Pi 5 release candidate refuses CK_PI5_STORE"
#endif
#ifdef CK_QEMU_UNSAFE_DMA
#error "Pi 5 release candidate refuses CK_QEMU_UNSAFE_DMA"
#endif
#ifdef CK_RNG_UNPROBED
#error "Pi 5 release candidate refuses CK_RNG_UNPROBED"
#endif
#ifdef CK_SB_E_TEST_KEYS
#error "Pi 5 release candidate refuses CK_SB_E_TEST_KEYS"
#endif
#ifdef CK_SEED0B_TEST_ANCHOR
#error "Pi 5 release candidate refuses CK_SEED0B_TEST_ANCHOR"
#endif
#ifdef CK_SELFTEST_EL2_FAULT
#error "Pi 5 release candidate refuses CK_SELFTEST_EL2_FAULT"
#endif
#ifdef CK_SERVICE_TEST_KEYS
#error "Pi 5 release candidate refuses CK_SERVICE_TEST_KEYS"
#endif
#ifdef CK_SLOT_TOOL_SELFTEST
#error "Pi 5 release candidate refuses CK_SLOT_TOOL_SELFTEST"
#endif
#ifdef CK_SLOT_UPDATE_KEYS
#error "Pi 5 release candidate refuses CK_SLOT_UPDATE_KEYS"
#endif
#ifdef CK_SMMU_MUTANT_
#error "Pi 5 release candidate refuses CK_SMMU_MUTANT_"
#endif
#ifdef CK_SMMU_MUTANT_BAD_L1_PTR
#error "Pi 5 release candidate refuses CK_SMMU_MUTANT_BAD_L1_PTR"
#endif
#ifdef CK_SMMU_MUTANT_L2_NO_FILL
#error "Pi 5 release candidate refuses CK_SMMU_MUTANT_L2_NO_FILL"
#endif
#ifdef CK_SMMU_MUTANT_SPAN_OVERFLOW
#error "Pi 5 release candidate refuses CK_SMMU_MUTANT_SPAN_OVERFLOW"
#endif
#ifdef CK_SMMU_MUTANT_UNGRANTED_VALID
#error "Pi 5 release candidate refuses CK_SMMU_MUTANT_UNGRANTED_VALID"
#endif
#ifdef CK_STORE_TEST_KEYS
#error "Pi 5 release candidate refuses CK_STORE_TEST_KEYS"
#endif
#ifdef CK_TEST_CONSOLE_MUTATION
#error "Pi 5 release candidate refuses CK_TEST_CONSOLE_MUTATION"
#endif
#ifdef CK_TEST_CONTINUITY
#error "Pi 5 release candidate refuses CK_TEST_CONTINUITY"
#endif
#ifdef CK_TEST_DISK_XLATE_BYPASS
#error "Pi 5 release candidate refuses CK_TEST_DISK_XLATE_BYPASS"
#endif
#ifdef CK_TEST_FWCFG_ARTIFACTS
#error "Pi 5 release candidate refuses CK_TEST_FWCFG_ARTIFACTS"
#endif
#ifdef CK_TEST_ROLLBACK
#error "Pi 5 release candidate refuses CK_TEST_ROLLBACK"
#endif
#ifdef CK_TEST_ROLLBACK_BAD_MAGIC
#error "Pi 5 release candidate refuses CK_TEST_ROLLBACK_BAD_MAGIC"
#endif
#ifdef CK_TEST_ROLLBACK_CPU_FAULT
#error "Pi 5 release candidate refuses CK_TEST_ROLLBACK_CPU_FAULT"
#endif
#ifdef CK_TEST_ROLLBACK_HANG
#error "Pi 5 release candidate refuses CK_TEST_ROLLBACK_HANG"
#endif
#ifdef CK_TEST_SLOT_KEY
#error "Pi 5 release candidate refuses CK_TEST_SLOT_KEY"
#endif
#ifdef CK_TEST_SMP_SKIP_CPU
#error "Pi 5 release candidate refuses CK_TEST_SMP_SKIP_CPU"
#endif
#ifdef CK_TEST_STALE_HANDOFF
#error "Pi 5 release candidate refuses CK_TEST_STALE_HANDOFF"
#endif
#ifdef CK_TEST_STORE_CRASH
#error "Pi 5 release candidate refuses CK_TEST_STORE_CRASH"
#endif
#ifdef CK_TEST_STORE_CRASH_MUTANT
#error "Pi 5 release candidate refuses CK_TEST_STORE_CRASH_MUTANT"
#endif
#ifdef CK_TEST_STORE_MUTANT_ACCEPT_BAD_ROOT_CRC
#error "Pi 5 release candidate refuses CK_TEST_STORE_MUTANT_ACCEPT_BAD_ROOT_CRC"
#endif
#ifdef CK_TEST_STORE_MUTANT_SKIP_ROOT_FLUSH
#error "Pi 5 release candidate refuses CK_TEST_STORE_MUTANT_SKIP_ROOT_FLUSH"
#endif
#ifdef CK_TEST_XHCI_MUTATION
#error "Pi 5 release candidate refuses CK_TEST_XHCI_MUTATION"
#endif
#ifdef CK_UEFI_VARS_SELF_TEST
#error "Pi 5 release candidate refuses CK_UEFI_VARS_SELF_TEST"
#endif
#ifdef CM_MUTANT_FIXED_AGENT
#error "Pi 5 release candidate refuses CM_MUTANT_FIXED_AGENT"
#endif
#ifdef CM_MUTANT_NO_WRITABLE_CHECK
#error "Pi 5 release candidate refuses CM_MUTANT_NO_WRITABLE_CHECK"
#endif
#ifdef CM_MUTANT_PROVISION_WITH_ROOT
#error "Pi 5 release candidate refuses CM_MUTANT_PROVISION_WITH_ROOT"
#endif
#ifdef CM_MUTANT_RESUME_NO_COMMIT
#error "Pi 5 release candidate refuses CM_MUTANT_RESUME_NO_COMMIT"
#endif
#ifdef CM_MUTANT_RESUME_PROVISIONS
#error "Pi 5 release candidate refuses CM_MUTANT_RESUME_PROVISIONS"
#endif
#ifdef CM_MUTANT_SPLIT_TXN
#error "Pi 5 release candidate refuses CM_MUTANT_SPLIT_TXN"
#endif
#ifdef CR_MUTANT_BARE_SHA256
#error "Pi 5 release candidate refuses CR_MUTANT_BARE_SHA256"
#endif
#ifdef CR_MUTANT_CHALLENGE_NO_ACTION
#error "Pi 5 release candidate refuses CR_MUTANT_CHALLENGE_NO_ACTION"
#endif
#ifdef CR_MUTANT_CHALLENGE_NO_DIGEST
#error "Pi 5 release candidate refuses CR_MUTANT_CHALLENGE_NO_DIGEST"
#endif
#ifdef CR_MUTANT_COMPARE_16
#error "Pi 5 release candidate refuses CR_MUTANT_COMPARE_16"
#endif
#ifdef CR_MUTANT_IGNORE_PREVIOUS
#error "Pi 5 release candidate refuses CR_MUTANT_IGNORE_PREVIOUS"
#endif
#ifdef CR_MUTANT_NO_DOMAIN
#error "Pi 5 release candidate refuses CR_MUTANT_NO_DOMAIN"
#endif
#ifdef CR_MUTANT_NO_ORPHAN_CHECK
#error "Pi 5 release candidate refuses CR_MUTANT_NO_ORPHAN_CHECK"
#endif
#ifdef CR_MUTANT_SKIP_CONFLICT
#error "Pi 5 release candidate refuses CR_MUTANT_SKIP_CONFLICT"
#endif
#ifdef CS_MUTANT_ACCEPT_FOREIGN
#error "Pi 5 release candidate refuses CS_MUTANT_ACCEPT_FOREIGN"
#endif
#ifdef CS_MUTANT_ACCEPT_FORK
#error "Pi 5 release candidate refuses CS_MUTANT_ACCEPT_FORK"
#endif
#ifdef CS_MUTANT_ACCEPT_TWO_ACTIVE
#error "Pi 5 release candidate refuses CS_MUTANT_ACCEPT_TWO_ACTIVE"
#endif
#ifdef CS_MUTANT_PROVISION_SPLIT_TXN
#error "Pi 5 release candidate refuses CS_MUTANT_PROVISION_SPLIT_TXN"
#endif
#ifdef CS_MUTANT_RESTORE_MINTS
#error "Pi 5 release candidate refuses CS_MUTANT_RESTORE_MINTS"
#endif
#ifdef CS_MUTANT_SKIP_INTENT_ID
#error "Pi 5 release candidate refuses CS_MUTANT_SKIP_INTENT_ID"
#endif
#ifdef RC_MUTANT_DEGRADED_IS_UNPROVISIONED
#error "Pi 5 release candidate refuses RC_MUTANT_DEGRADED_IS_UNPROVISIONED"
#endif
#ifdef RC_MUTANT_INSPECT_WRITES
#error "Pi 5 release candidate refuses RC_MUTANT_INSPECT_WRITES"
#endif
#ifdef RC_MUTANT_PROVISION_QUALIFICATION
#error "Pi 5 release candidate refuses RC_MUTANT_PROVISION_QUALIFICATION"
#endif
#ifdef RC_MUTANT_REPAIR_ACTIVE
#error "Pi 5 release candidate refuses RC_MUTANT_REPAIR_ACTIVE"
#endif
#ifdef RC_MUTANT_REPAIR_NO_IDENTITY
#error "Pi 5 release candidate refuses RC_MUTANT_REPAIR_NO_IDENTITY"
#endif

#endif
