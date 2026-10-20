//! Device models shared by every backend that traps MMIO itself (own KVM VMM today,
//! the WHPX API loop of ledger L261 next).

pub mod virtio_blk;
