//! Shared test-only fault device for the System Store v1 crash harness.
//!
//! This models the property the two-flush commit protocol depends on:
//! writes land in volatile state, `flush()` makes volatile state durable, and
//! simulated power loss discards unflushed state. A torn write models a partial
//! media write that reaches durable storage immediately and cannot be repaired
//! by a later flush.
//!
//! No production `StoreDevice` semantics are involved.
#![allow(dead_code)]

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use aienos_kernel::store::v1::{
    object_unit_count, Catalog, CommitRecord, ObjectId, Superblock, STORE_UNIT_BYTES,
};
use aienos_kernel::store::{Checkpoint, CheckpointHook, Store, StoreDevice};

pub const UNIT: usize = STORE_UNIT_BYTES;
pub const STORE_UUID: [u8; 16] = [0x5A; 16];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FaultError;

impl core::fmt::Display for FaultError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "volatile fault device I/O error")
    }
}

/// Volatile + durable media state with deterministic, enumerable faults.
#[derive(Debug)]
pub struct FaultState {
    pub durable: Vec<u8>,
    pub region_units: u64,
    pending: BTreeMap<u64, [u8; UNIT]>,
    pub write_count: usize,
    pub flush_count: usize,
    /// (nth write, byte offset): that write tears at `offset` and the prefix
    /// goes straight to durable media; the remainder is lost.
    pub tear: Option<(usize, usize)>,
    /// nth write returns an I/O error.
    pub fail_write: Option<usize>,
    /// nth flush returns an I/O error.
    pub fail_flush: Option<usize>,
}

impl FaultState {
    pub fn durable_bytes(&self) -> Vec<u8> {
        self.durable.clone()
    }
    pub fn power_loss(&mut self) {
        self.pending.clear();
    }
    pub fn pending_len(&self) -> usize {
        self.pending.len()
    }
    /// Reset deterministic fault counters so a new phase starts at write 1.
    pub fn reset_counters(&mut self) {
        self.write_count = 0;
        self.flush_count = 0;
    }
}

#[derive(Debug)]
pub struct FaultStoreDevice {
    state: Rc<RefCell<FaultState>>,
}

impl FaultStoreDevice {
    pub fn new(durable: Vec<u8>, region_units: u64) -> Self {
        assert_eq!(durable.len(), region_units as usize * UNIT, "image size");
        Self {
            state: Rc::new(RefCell::new(FaultState {
                durable,
                region_units,
                pending: BTreeMap::new(),
                write_count: 0,
                flush_count: 0,
                tear: None,
                fail_write: None,
                fail_flush: None,
            })),
        }
    }
    pub fn state(&self) -> Rc<RefCell<FaultState>> {
        self.state.clone()
    }
}

impl StoreDevice for FaultStoreDevice {
    type Error = FaultError;

    fn region_units(&self) -> u64 {
        self.state.borrow().region_units
    }
    fn read_unit(
        &mut self,
        unit: u64,
        out: &mut [u8; STORE_UNIT_BYTES],
    ) -> Result<(), Self::Error> {
        let st = self.state.borrow();
        if let Some(bytes) = st.pending.get(&unit) {
            out.copy_from_slice(bytes);
            return Ok(());
        }
        let off = unit as usize * UNIT;
        if off + UNIT > st.durable.len() {
            return Err(FaultError);
        }
        out.copy_from_slice(&st.durable[off..off + UNIT]);
        Ok(())
    }
    fn write_unit(&mut self, unit: u64, bytes: &[u8; STORE_UNIT_BYTES]) -> Result<(), Self::Error> {
        let mut st = self.state.borrow_mut();
        st.write_count += 1;
        let k = st.write_count;
        if st.fail_write == Some(k) {
            return Err(FaultError);
        }
        if let Some((target, offset)) = st.tear {
            if target == k {
                let off = offset.min(UNIT);
                let base = unit as usize * UNIT;
                st.durable[base..base + off].copy_from_slice(&bytes[..off]);
                return Ok(());
            }
        }
        st.pending.insert(unit, *bytes);
        Ok(())
    }
    fn flush(&mut self) -> Result<(), Self::Error> {
        let mut st = self.state.borrow_mut();
        st.flush_count += 1;
        if st.fail_flush == Some(st.flush_count) {
            return Err(FaultError);
        }
        let pending = std::mem::take(&mut st.pending);
        for (unit, bytes) in pending {
            let base = unit as usize * UNIT;
            st.durable[base..base + UNIT].copy_from_slice(&bytes);
        }
        Ok(())
    }
}

/// Fires at a chosen checkpoint: discards volatile state (power loss) and
/// aborts the transaction by unwinding. Not a crash test by itself — the caller
/// must reopen from durable bytes.
pub struct CrashHook {
    pub state: Rc<RefCell<FaultState>>,
    pub target: Checkpoint,
    pub fired: Rc<RefCell<bool>>,
}

impl CrashHook {
    pub fn new(state: Rc<RefCell<FaultState>>, target: Checkpoint) -> Self {
        Self {
            state,
            target,
            fired: Rc::new(RefCell::new(false)),
        }
    }
}

impl CheckpointHook for CrashHook {
    fn on_checkpoint(&mut self, checkpoint: Checkpoint) {
        if checkpoint == self.target {
            self.state.borrow_mut().power_loss();
            *self.fired.borrow_mut() = true;
            panic!("simulated power loss at {checkpoint:?}");
        }
    }
}

/// A genesis image: empty catalog, generation 1, both superblocks equivalent.
pub fn genesis(region_units: u64) -> Vec<u8> {
    assert!(region_units >= 4);
    let mut img = vec![0u8; region_units as usize * UNIT];

    let catalog = Catalog {
        entries: Vec::new(),
    };
    let catalog_bytes = catalog.encode().expect("catalog encode");
    let catalog_id = ObjectId::calculate(1, 1, &catalog_bytes).expect("catalog id");
    let catalog_units = object_unit_count(catalog_bytes.len() as u64).expect("units");
    let catalog_first = 2u64;
    let commit_unit = catalog_first + u64::from(catalog_units);
    let high_water = commit_unit + 1;

    let commit = CommitRecord {
        store_uuid: STORE_UUID,
        region_units,
        generation: 1,
        previous_generation: 0,
        previous_commit_id: ObjectId([0u8; 32]),
        previous_catalog_id: ObjectId([0u8; 32]),
        catalog_id,
        catalog_first_unit: catalog_first,
        catalog_byte_length: catalog_bytes.len() as u64,
        catalog_unit_count: catalog_units,
        catalog_entry_count: 0,
        committed_high_water: high_water,
    };
    let commit_bytes = commit.encode().expect("commit encode");
    let commit_id = commit.object_id().expect("commit id");

    let superblock = |slot: u32| Superblock {
        store_uuid: STORE_UUID,
        slot_id: slot,
        region_units,
        generation: 1,
        commit_record_id: commit_id,
        commit_record_unit: commit_unit,
        catalog_id,
        catalog_first_unit: catalog_first,
        catalog_byte_length: catalog_bytes.len() as u64,
        catalog_unit_count: catalog_units,
        catalog_entry_count: 0,
        committed_high_water: high_water,
    };
    let sb_a = superblock(0).encode().expect("sb a");
    let sb_b = superblock(1).encode().expect("sb b");
    img[0..UNIT].copy_from_slice(&sb_a);
    img[UNIT..2 * UNIT].copy_from_slice(&sb_b);
    let cf = catalog_first as usize * UNIT;
    img[cf..cf + catalog_bytes.len()].copy_from_slice(&catalog_bytes);
    let cu = commit_unit as usize * UNIT;
    img[cu..cu + commit_bytes.len()].copy_from_slice(&commit_bytes);
    img
}

pub fn object(kind: u16, bytes: &'static [u8]) -> aienos_kernel::store::ObjectInput<'static> {
    aienos_kernel::store::ObjectInput {
        kind,
        version: 1,
        bytes,
    }
}

pub fn object_ref<'a>(kind: u16, bytes: &'a [u8]) -> aienos_kernel::store::ObjectInput<'a> {
    aienos_kernel::store::ObjectInput {
        kind,
        version: 1,
        bytes,
    }
}

/// Reopen a store from the current durable bytes (volatile state must already
/// have been discarded). Shares the same state so later flushes/power-loss apply.
pub fn reopen(state: &Rc<RefCell<FaultState>>) -> Result<Store<FaultStoreDevice>, MountErrorRepr> {
    let dev = FaultStoreDevice {
        state: state.clone(),
    };
    Store::open(dev).map_err(MountErrorRepr::from)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MountErrorRepr {
    Io,
    Unformatted,
    ForeignOrUnknown,
    UnsupportedVersion,
    CorruptRecoveryRequired,
    ConflictingRoots,
    InconsistentHistory,
}

impl From<aienos_kernel::store::MountError> for MountErrorRepr {
    fn from(err: aienos_kernel::store::MountError) -> Self {
        use aienos_kernel::store::MountError as M;
        match err {
            M::Io => Self::Io,
            M::Unformatted => Self::Unformatted,
            M::ForeignOrUnknown => Self::ForeignOrUnknown,
            M::UnsupportedVersion => Self::UnsupportedVersion,
            M::CorruptRecoveryRequired => Self::CorruptRecoveryRequired,
            M::ConflictingRoots => Self::ConflictingRoots,
            M::InconsistentHistory => Self::InconsistentHistory,
        }
    }
}
