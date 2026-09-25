//! Task 5: failed transaction write/flush poisons the live Store and never
//! activates the pending generation; reopening recovers the last commit.
mod common;

use aienos_kernel::store::v1::ObjectId;
use aienos_kernel::store::{MountState, StoreError};
use common::{genesis, object, object_ref, reopen, FaultStoreDevice};

const REGION: u64 = 64;
const PENDING: &[u8] = b"pending-object";

fn seeded() -> std::rc::Rc<std::cell::RefCell<common::FaultState>> {
    let dev = FaultStoreDevice::new(genesis(REGION), REGION);
    let state = dev.state();
    let mut store = aienos_kernel::store::Store::open(dev).expect("open");
    store.transact(&[object(3, b"committed-2")]).expect("seed");
    state.borrow_mut().reset_counters();
    state
}

fn assert_poison_and_recover(fail_write: Option<usize>, fail_flush: Option<usize>) {
    let state = seeded();
    let mut store = reopen(&state).expect("reopen");
    {
        let mut st = state.borrow_mut();
        st.fail_write = fail_write;
        st.fail_flush = fail_flush;
    }
    let first = store.transact(&[object(3, PENDING)]);
    assert_eq!(first, Err(StoreError::Io), "first op must fail with Io");
    // Poisoned: a subsequent operation refuses rather than half-committing.
    assert_eq!(
        store.transact(&[object(3, PENDING)]),
        Err(StoreError::NeedsReopen)
    );
    state.borrow_mut().power_loss();
    let mut reopened = reopen(&state).expect("reopen after failure");
    assert_eq!(reopened.generation(), 2);
    assert_eq!(reopened.mount_state(), MountState::Valid);
    assert_eq!(
        reopened.read_object(ObjectId::calculate(3, 1, PENDING).unwrap()),
        Err(StoreError::InvalidObject),
        "pending generation must not be activated"
    );
    assert_eq!(
        reopened.read_object(ObjectId::calculate(3, 1, b"committed-2").unwrap()),
        Ok(b"committed-2".to_vec())
    );
}

#[test]
fn failed_write_poisons_and_preserves_last_commit() {
    assert_poison_and_recover(Some(1), None);
}

#[test]
fn failed_flush_poisons_and_preserves_last_commit() {
    assert_poison_and_recover(None, Some(1));
}

#[test]
fn failed_superblock_write_poisons_and_preserves_last_commit() {
    assert_poison_and_recover(Some(4), None);
}

#[test]
fn object_ref_shape_is_usable() {
    let buf = [1u8, 2, 3];
    let _ = object_ref(3, &buf);
}
