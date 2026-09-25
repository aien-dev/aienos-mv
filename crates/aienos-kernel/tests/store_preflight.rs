//! Task 6: preflight failures produce zero persistent writes.
mod common;

use aienos_kernel::store::{ObjectInput, StoreError};
use common::{genesis, object, object_ref, FaultStoreDevice};

fn write_count(state: &std::rc::Rc<std::cell::RefCell<common::FaultState>>) -> usize {
    state.borrow().write_count
}

#[test]
fn nospace_writes_nothing() {
    // Region of exactly 4 units is consumed by genesis metadata + commit.
    let dev = FaultStoreDevice::new(genesis(4), 4);
    let state = dev.state();
    let mut store = aienos_kernel::store::Store::open(dev).expect("open");
    state.borrow_mut().reset_counters();
    assert_eq!(store.transact(&[object(3, b"x")]), Err(StoreError::NoSpace));
    assert_eq!(write_count(&state), 0, "NoSpace must not touch media");
    assert_eq!(state.borrow().flush_count, 0, "NoSpace must not flush");
}

#[test]
fn transaction_limit_writes_nothing() {
    let dev = FaultStoreDevice::new(genesis(64), 64);
    let state = dev.state();
    let mut store = aienos_kernel::store::Store::open(dev).expect("open");
    state.borrow_mut().reset_counters();

    let buffers: Vec<Vec<u8>> = (0..257).map(|i| format!("obj-{i}").into_bytes()).collect();
    let inputs: Vec<ObjectInput<'_>> = buffers.iter().map(|b| object_ref(3, b)).collect();
    assert_eq!(store.transact(&inputs), Err(StoreError::TransactionLimit));
    assert_eq!(write_count(&state), 0, "limit must not touch media");
    assert_eq!(state.borrow().flush_count, 0, "limit must not flush");
}
