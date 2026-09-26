use aienos_kernel::continuity::{
    child_branch_id, Branch, CortexRecord, Epistemic, ProvisionSource, Update,
};
use aienos_kernel::security::{
    commit_authenticated, provision_authenticated, resolve_authenticated, RollbackAnchor,
    SecurityError,
};
use aienos_kernel::store::engine::{Store, StoreDevice};
use aienos_kernel::store::genesis::genesis_units;
use aienos_kernel::store::v1::STORE_UNIT_BYTES;

const UUID: [u8; 16] = [0x5a; 16];
const AGENT: [u8; 32] = [0x11; 32];
const K_VOL: [u8; 32] = [0x77; 32];
const TPM_KEK: [u8; 32] = [0x88; 32];
const RECOVERY_KEK: [u8; 32] = [0x99; 32];
const WRONG_KEK: [u8; 32] = [0xee; 32];

#[derive(Clone)]
struct Mem {
    units: Vec<[u8; STORE_UNIT_BYTES]>,
}

impl StoreDevice for Mem {
    type Error = ();
    fn region_units(&self) -> u64 {
        self.units.len() as u64
    }
    fn read_unit(
        &mut self,
        unit: u64,
        out: &mut [u8; STORE_UNIT_BYTES],
    ) -> core::result::Result<(), ()> {
        *out = *self.units.get(unit as usize).ok_or(())?;
        Ok(())
    }
    fn write_unit(
        &mut self,
        unit: u64,
        bytes: &[u8; STORE_UNIT_BYTES],
    ) -> core::result::Result<(), ()> {
        *self.units.get_mut(unit as usize).ok_or(())? = *bytes;
        Ok(())
    }
    fn flush(&mut self) -> core::result::Result<(), ()> {
        Ok(())
    }
}

fn formatted_store() -> Mem {
    let mut units = vec![[0u8; STORE_UNIT_BYTES]; 256];
    for (i, u) in genesis_units(UUID, 256).iter().enumerate() {
        units[i] = *u;
    }
    Mem { units }
}

fn fact(text: &str) -> CortexRecord {
    CortexRecord {
        status: Epistemic::DirectObservation,
        evidence_hash: [0x42; 32],
        statement: text.as_bytes().to_vec(),
    }
}

#[test]
fn test_m5_provision_and_unseal_both_slots() {
    let dev = formatted_store();
    let mut store = Store::open(dev).expect("store opens");

    let prov = provision_authenticated(
        &mut store,
        AGENT,
        ProvisionSource::Operator,
        &K_VOL,
        Some(&TPM_KEK),
        &RECOVERY_KEK,
    )
    .expect("provision authenticated succeeds");

    assert_eq!(prov.root.agent_id, AGENT);
    assert_eq!(prov.manifest.sequence, 1);
    assert_eq!(prov.state.branches.len(), 1);

    // Re-open and unseal via Slot 0 (TPM KEK)
    let dev = store.into_device();
    let mut store = Store::open(dev.clone()).expect("store opens");
    let resolved_tpm = resolve_authenticated(&mut store, &TPM_KEK, None)
        .expect("unseal via Slot 0 (TPM KEK) succeeds");

    assert_eq!(resolved_tpm.root.agent_id, AGENT);
    assert_eq!(resolved_tpm.manifest.sequence, 1);
    assert_eq!(resolved_tpm.state.branches.len(), 1);

    // Re-open and unseal via Slot 1 (Recovery KEK)
    let dev = store.into_device();
    let mut store = Store::open(dev).expect("store opens");
    let resolved_rec = resolve_authenticated(&mut store, &RECOVERY_KEK, None)
        .expect("unseal via Slot 1 (Recovery KEK) succeeds");

    assert_eq!(resolved_rec.root.agent_id, AGENT);
    assert_eq!(resolved_rec.manifest.sequence, 1);
}

#[test]
fn test_m5_wrong_key_fails_closed() {
    let dev = formatted_store();
    let mut store = Store::open(dev).expect("store opens");

    provision_authenticated(
        &mut store,
        AGENT,
        ProvisionSource::Operator,
        &K_VOL,
        Some(&TPM_KEK),
        &RECOVERY_KEK,
    )
    .expect("provision succeeds");

    let dev = store.into_device();
    let mut store = Store::open(dev).expect("store opens");

    let res = resolve_authenticated(&mut store, &WRONG_KEK, None);
    assert_eq!(res, Err(SecurityError::AuthenticationFailed));
}

#[test]
fn test_m5_tampered_envelope_fails_closed() {
    let dev = formatted_store();
    let mut store = Store::open(dev).expect("store opens");

    let prov = provision_authenticated(
        &mut store,
        AGENT,
        ProvisionSource::Operator,
        &K_VOL,
        Some(&TPM_KEK),
        &RECOVERY_KEK,
    )
    .expect("provision succeeds");

    let subkeys = aienos_kernel::security::derive_subkeys(&K_VOL);
    let store_uuid = store.store_uuid();

    // 1. Read valid sealed AgentRoot envelope
    let mut root_envelope = store.read_object(prov.root_id).expect("read root object");
    assert!(aienos_kernel::security::unseal_application_payload(
        &subkeys,
        &store_uuid,
        aienos_kernel::continuity::KIND_AGENT_ROOT,
        1,
        &root_envelope,
    )
    .is_ok());

    // 2. Tamper with 1 byte of ciphertext inside the envelope
    root_envelope[70] ^= 0x55;
    let res = aienos_kernel::security::unseal_application_payload(
        &subkeys,
        &store_uuid,
        aienos_kernel::continuity::KIND_AGENT_ROOT,
        1,
        &root_envelope,
    );
    assert_eq!(res, Err(SecurityError::AuthenticationFailed));

    // 3. Tamper with 1 byte of the Polyval authentication tag at the end of chunk
    let tag_offset = root_envelope.len() - 1;
    root_envelope[tag_offset] ^= 0x80;
    let res = aienos_kernel::security::unseal_application_payload(
        &subkeys,
        &store_uuid,
        aienos_kernel::continuity::KIND_AGENT_ROOT,
        1,
        &root_envelope,
    );
    assert_eq!(res, Err(SecurityError::AuthenticationFailed));

    // 4. Physical media tamper test: mutating raw on-disk bytes of target_unit
    let entry = store
        .catalog()
        .entries
        .iter()
        .find(|e| e.object_id == prov.root_id)
        .expect("root entry exists");
    let target_unit = entry.first_unit as usize;

    let mut dev = store.into_device();
    dev.units[target_unit][80] ^= 0xaa;
    let store_res = Store::open(dev);
    // Either open fails or active root is rejected and falls back to unprovisioned peer
    if let Ok(mut s) = store_res {
        let res = resolve_authenticated(&mut s, &TPM_KEK, None);
        assert!(res.is_err());
    } else {
        assert!(store_res.is_err());
    }
}

#[test]
fn test_m5_tampered_security_manifest_mac_fails_closed() {
    let subkeys = aienos_kernel::security::derive_subkeys(&K_VOL);

    // Create a validly formatted SecurityManifest but with a forged/wrong MAC
    let mut manifest = aienos_kernel::security::SecurityManifest {
        format_version: 1,
        flags: 0,
        reserved: 0,
        store_uuid: UUID,
        generation: 1,
        security_sequence: 1,
        epoch: 1,
        key_epoch: 1,
        previous_security_manifest_id: aienos_kernel::store::v1::ObjectId([0; 32]),
        keyslot_manifest_id: aienos_kernel::store::v1::ObjectId([1; 32]),
        agent_root_id: aienos_kernel::store::v1::ObjectId([2; 32]),
        continuity_manifest_id: aienos_kernel::store::v1::ObjectId([3; 32]),
        migration_manifest_id: aienos_kernel::store::v1::ObjectId([0; 32]),
        root_mac: [0xba; 32],
    };

    // Correct sign
    manifest.sign(&subkeys.k_root_auth);
    assert!(manifest.verify(&subkeys.k_root_auth));

    // Tampered MAC must fail verify in constant time
    manifest.root_mac[15] ^= 0xff;
    assert!(!manifest.verify(&subkeys.k_root_auth));
}

#[test]
fn test_m5_commit_authenticated_branches_and_cortex() {
    let dev = formatted_store();
    let mut store = Store::open(dev).expect("store opens");

    let prov = provision_authenticated(
        &mut store,
        AGENT,
        ProvisionSource::Operator,
        &K_VOL,
        Some(&TPM_KEK),
        &RECOVERY_KEK,
    )
    .expect("provision succeeds");

    let root_branch = prov.state.branches[0].id;
    let child_id = child_branch_id(&root_branch, 0);

    let mut branches = prov.state.branches.clone();
    branches[0].forks = 1;
    branches.push(Branch {
        id: child_id,
        parent: Some(root_branch),
        depth: 1,
        forks: 0,
    });
    branches.sort_by_key(|b| b.id);

    let new_state = aienos_kernel::continuity::AgentState {
        agent_id: AGENT,
        written_at: 2,
        branches,
    };

    let update = Update {
        state: Some(new_state),
        cortex: vec![
            fact("DGX Spark Tegra UART initialized at 0x16A00000"),
            fact("SMMUv3 DMA stream table configured in fail-closed mode"),
        ],
    };

    let committed = commit_authenticated(&mut store, &K_VOL, update, 1, true)
        .expect("commit authenticated succeeds");

    assert_eq!(committed.manifest.sequence, 2);
    assert_eq!(committed.state.branches.len(), 2);
    assert_eq!(committed.cortex.len(), 2);
    assert_eq!(
        committed.cortex[0].statement,
        b"DGX Spark Tegra UART initialized at 0x16A00000"
    );

    // Power cycle and resolve
    let dev = store.into_device();
    let mut store = Store::open(dev).expect("store opens");
    let resolved =
        resolve_authenticated(&mut store, &TPM_KEK, None).expect("resolve after commit succeeds");

    assert_eq!(resolved.manifest.sequence, 2);
    assert_eq!(resolved.state.branches.len(), 2);
    assert_eq!(resolved.cortex.len(), 2);
}

#[test]
fn test_m5_anti_rollback_enforcement() {
    let dev = formatted_store();
    let mut store = Store::open(dev).expect("store opens");

    provision_authenticated(
        &mut store,
        AGENT,
        ProvisionSource::Operator,
        &K_VOL,
        Some(&TPM_KEK),
        &RECOVERY_KEK,
    )
    .expect("provision succeeds");

    // Snapshot state at Epoch 1
    let epoch1_dev = store.into_device();
    let mut store = Store::open(epoch1_dev.clone()).expect("store opens");

    // Commit update advancing to Epoch 2
    let update = Update {
        state: None,
        cortex: vec![fact("Advancing to epoch 2")],
    };
    commit_authenticated(&mut store, &K_VOL, update, 1, true).expect("advance to epoch 2 succeeds");

    // Inspect the new SecurityManifest to construct the RollbackAnchor at Epoch 2
    let sec_entry = store
        .catalog()
        .entries
        .iter()
        .find(|e| e.kind == 22)
        .expect("security manifest exists");
    let sec_bytes = store.read_object(sec_entry.object_id).expect("read sec");
    let sec = aienos_kernel::security::SecurityManifest::decode(&sec_bytes).expect("decode sec");

    let anchor_epoch2 = RollbackAnchor {
        store_uuid: UUID,
        epoch: sec.epoch,
        commit_record_id: store.commit_record_id(),
        security_root_digest: sec.root_mac,
    };
    assert_eq!(anchor_epoch2.epoch, 2);

    // 1. Current store at Epoch 2 verifies against anchor_epoch2
    let res = resolve_authenticated(&mut store, &TPM_KEK, Some(&anchor_epoch2));
    assert!(res.is_ok());

    // 2. Replay attack: open the Epoch 1 disk image against anchor_epoch2
    let mut replayed_store = Store::open(epoch1_dev).expect("replayed store opens structurally");
    let res = resolve_authenticated(&mut replayed_store, &TPM_KEK, Some(&anchor_epoch2));

    // Stale disk epoch (1 < 2) detected as rollback
    assert!(matches!(res, Err(SecurityError::RollbackDetected(_))));
}

#[test]
fn test_m5_store_v1_permanence_unmutated() {
    let dev = formatted_store();
    let mut store = Store::open(dev).expect("store opens");

    provision_authenticated(
        &mut store,
        AGENT,
        ProvisionSource::Operator,
        &K_VOL,
        Some(&TPM_KEK),
        &RECOVERY_KEK,
    )
    .expect("provision succeeds");

    let dev = store.into_device();
    // Superblock is at unit 0 (slot 0) or unit 1 (slot 1)
    let sb_bytes = dev.units[1]; // active slot is 1 after first transaction
                                 // ADR 0015 Permanence: bytes 172..4096 of the superblock must be strictly 0
    assert!(sb_bytes[172..4096].iter().all(|b| *b == 0));
}
