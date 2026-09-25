//! Task 2 + Task 3: checkpoint crash matrix and torn-write matrix.
//!
//! For each canonical checkpoint the transaction is aborted with a simulated
//! power loss (volatile state discarded), then a fresh Store is reconstructed
//! from durable bytes and classified. The recovered generation must be N or
//! N+1 and the root must be self-consistent (no mixed logical generation).
mod common;

use std::cell::RefCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;

use aienos_kernel::store::{Checkpoint, MountState, ObjectInput};
use common::{
    genesis, object, reopen, CrashHook, FaultState, FaultStoreDevice, MountErrorRepr, UNIT,
};

const REGION: u64 = 64;
const OLD: u64 = 2;
const NEW: u64 = 3;

/// Semantic byte lengths of the three append units for a one-object commit:
/// payload ("torn-object" = 11 B), catalog (16 + 64 = 80 B), commit record (200 B).
// The new catalog holds the seed entry plus the new one: 16 + 2*64 = 144 bytes.
const SEMANTIC_LEN: [(&str, usize); 3] = [("payload", 11), ("catalog", 144), ("commit", 200)];

fn seeded_state() -> Rc<RefCell<FaultState>> {
    let dev = FaultStoreDevice::new(genesis(REGION), REGION);
    let state = dev.state();
    let mut store = aienos_kernel::store::Store::open(dev).expect("open genesis");
    store
        .transact(&[object(3, b"genesis-object")])
        .expect("seed txn");
    assert_eq!(store.generation(), OLD);
    state.borrow_mut().reset_counters();
    state
}

fn crash_at(checkpoint: Checkpoint) -> (u64, MountState) {
    let state = seeded_state();
    let mut store = reopen(&state).expect("reopen seeded");
    let hook = CrashHook::new(state.clone(), checkpoint);
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let _ = store.transact_with_hook(&[object(3, b"crash-object")], hook);
    }));
    state.borrow_mut().power_loss();
    let reopened = reopen(&state).expect("reopen after crash");
    (reopened.generation(), reopened.mount_state())
}

// ---------------------------------------------------------------- Task 2
#[test]
fn task2_checkpoint_crash_preserves_old_or_new_generation() {
    let mut failures = Vec::new();
    for checkpoint in Checkpoint::ORDERED {
        let (generation, state) = crash_at(checkpoint);
        let ok = (generation == OLD || generation == NEW) && state == MountState::Valid;
        if ok {
            println!("PASS  checkpoint {checkpoint:<18} -> generation {generation} {state:?}");
        } else {
            failures.push((checkpoint, generation, state));
        }
    }
    assert!(failures.is_empty(), "OLD-or-NEW violations: {failures:?}");
}

// ---------------------------------------------------------------- Task 3
#[test]
fn task3_torn_append_writes_keep_old_generation_or_commit_cleanly() {
    // Write order for a one-object commit: 1=payload, 2=catalog, 3=commit, 4=superblock.
    let offsets: [usize; 7] = [1, 7, 64, 100, 160, 2048, 4095];
    let mut failures = Vec::new();
    for (index, (label, semantic_len)) in SEMANTIC_LEN.iter().enumerate() {
        let write = index + 1;
        for offset in offsets {
            let state = seeded_state();
            state.borrow_mut().tear = Some((write, offset));
            let mut store = reopen(&state).expect("reopen seeded");
            let _ = store.transact(&[object(3, b"torn-object")]);
            state.borrow_mut().power_loss();

            let expected = if offset < *semantic_len { OLD } else { NEW };
            match reopen(&state) {
                Ok(reopened) if reopened.generation() == expected => {
                    println!("PASS  tear {label:<8} offset={offset:<5} -> generation {expected}");
                }
                Ok(reopened) => failures.push((
                    label,
                    offset,
                    format!("expected {expected}, got {}", reopened.generation()),
                )),
                Err(err) => failures.push((label, offset, format!("open {err:?}"))),
            }
        }
    }
    assert!(
        failures.is_empty(),
        "torn append-write failures: {failures:?}"
    );
}

/// Tear the inactive Superblock write across its interesting byte ranges.
/// This is a CHARACTERIZATION test: a torn inactive superblock is CRC-invalid
/// and ADR 0015 classifies it Corrupt/RecoveryRequired even though the active
/// slot holds a valid previous root. OLD-or-NEW is NOT claimed here; recovery
/// semantics are not altered.
#[test]
fn task3_torn_inactive_superblock_characterization() {
    let offsets: [usize; 10] = [1, 50, 100, 167, 168, 171, 172, 200, 2048, 4095];
    let mut corrupt = 0usize;
    let mut failures = Vec::new();
    for offset in offsets {
        let state = seeded_state();
        state.borrow_mut().tear = Some((4, offset));
        let mut store = reopen(&state).expect("reopen seeded");
        let _ = store.transact(&[object(3, b"torn-superblock-object")]);
        state.borrow_mut().power_loss();
        match reopen(&state) {
            Ok(reopened) => {
                let g = reopened.generation();
                println!("CHAR  superblock offset={offset:<5} -> generation {g}");
                if g != OLD && g != NEW {
                    failures.push((offset, format!("mixed generation {g}")));
                }
            }
            Err(MountErrorRepr::CorruptRecoveryRequired) => {
                corrupt += 1;
                println!("CHAR  superblock offset={offset:<5} -> CorruptRecoveryRequired (hazard)");
            }
            Err(other) => failures.push((offset, format!("unexpected {other:?}"))),
        }
    }
    assert!(
        failures.is_empty(),
        "characterization surprises: {failures:?}"
    );
    assert!(
        corrupt > 0,
        "expected at least one CorruptRecoveryRequired outcome to expose the atomicity dependency"
    );
}

#[test]
fn task3_untorn_superblock_commits_new_generation() {
    let state = seeded_state();
    state.borrow_mut().tear = Some((4, UNIT)); // whole unit lands
    let mut store = reopen(&state).expect("reopen seeded");
    store.transact(&[object(3, b"eager-object")]).expect("txn");
    state.borrow_mut().power_loss();
    assert_eq!(reopen(&state).expect("reopen").generation(), NEW);
}

#[test]
fn object_input_type_is_usable() {
    let _: ObjectInput<'_> = object(3, b"x");
}
