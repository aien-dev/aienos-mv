//! Task 7: after an uncommitted/failed transaction, reopening recovers the last
//! committed generation and the uncommitted tail may be reused without
//! corrupting committed history.
mod common;

use aienos_kernel::store::v1::ObjectId;
use aienos_kernel::store::{MountState, StoreError};
use common::{genesis, object, reopen, FaultStoreDevice};

const REGION: u64 = 64;

fn oid(bytes: &'static [u8]) -> ObjectId {
    ObjectId::calculate(3, 1, bytes).unwrap()
}

#[test]
fn tail_reuse_preserves_committed_objects_and_history() {
    let dev = FaultStoreDevice::new(genesis(REGION), REGION);
    let state = dev.state();
    let mut store = aienos_kernel::store::Store::open(dev).expect("open");
    store.transact(&[object(3, b"committed-2")]).expect("seed");
    assert_eq!(store.generation(), 2);

    // Fail the Superblock write so payload/catalog/commit land but the root is
    // never activated: an uncommitted tail exists on media.
    state.borrow_mut().reset_counters();
    state.borrow_mut().fail_write = Some(4);
    assert_eq!(
        store.transact(&[object(3, b"pending-3")]),
        Err(StoreError::Io)
    );
    state.borrow_mut().power_loss();

    let mut reopened = reopen(&state).expect("reopen");
    assert_eq!(reopened.generation(), 2);
    assert_eq!(reopened.mount_state(), MountState::Valid);
    assert_eq!(
        reopened.read_object(oid(b"committed-2")).unwrap(),
        b"committed-2"
    );

    // Reuse the uncommitted tail for a fresh commit.
    reopened
        .transact(&[object(3, b"second-3")])
        .expect("retry txn");
    assert_eq!(reopened.generation(), 3);
    assert_eq!(reopened.mount_state(), MountState::Valid);
    assert_eq!(
        reopened.read_object(oid(b"committed-2")).unwrap(),
        b"committed-2"
    );
    assert_eq!(reopened.read_object(oid(b"second-3")).unwrap(), b"second-3");
}
