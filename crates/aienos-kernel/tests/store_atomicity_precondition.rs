//! Task 4: test-side model of the physical precondition a Store Superblock
//! write depends on. Pure characterization; no production NVMe code.
//!
//! NVMe atomic-unit fields are 0's based. The effective power-fail atomic unit
//! is the namespace guarantee when one applies, otherwise the controller AWUPF.
//! A write is atomic only if it fits inside that unit AND does not cross a
//! power-fail atomic boundary.

/// Decoded atomicity fields (raw register values, 0-based as the spec defines).
#[derive(Debug, Clone, Copy)]
struct Atomicity {
    lba_bytes: u32,
    /// Identify Controller AWUPF raw (0-based). None = not reported.
    awupf_raw: Option<u16>,
    /// Identify Namespace NAWUPF raw (0-based). None = namespace does not apply.
    nawupf_raw: Option<u16>,
    /// Namespace Atomic Boundary Size Power Fail raw (0-based blocks). 0 = none.
    nabspf_raw: u16,
    /// Namespace Atomic Boundary Offset in logical blocks.
    nabo_blocks: u64,
}

impl Atomicity {
    /// Effective guaranteed atomic size in logical blocks.
    fn effective_blocks(self) -> u64 {
        match self.nawupf_raw {
            Some(raw) => u64::from(raw) + 1,
            None => u64::from(self.awupf_raw.unwrap_or(0)) + 1,
        }
    }
    /// Atomic boundary size in blocks (0 = no boundary constraint).
    fn boundary_blocks(self) -> u64 {
        if self.nabspf_raw == 0 {
            0
        } else {
            u64::from(self.nabspf_raw) + 1
        }
    }
    /// Region index of a logical block under the atomic boundary.
    fn region(self, block: u64) -> i128 {
        let size = self.boundary_blocks();
        if size == 0 {
            return 0;
        }
        let offset = (self.nabo_blocks % size) as i128;
        (block as i128 - offset).div_euclid(size as i128)
    }
    fn crosses_boundary(self, start_lba: u64, blocks: u64) -> bool {
        if self.boundary_blocks() == 0 || blocks == 0 {
            return false;
        }
        self.region(start_lba) != self.region(start_lba + blocks - 1)
    }
    /// True when a `blocks`-long write at `start_lba` is power-fail atomic.
    fn write_is_atomic(self, start_lba: u64, blocks: u64) -> bool {
        blocks != 0
            && blocks <= self.effective_blocks()
            && !self.crosses_boundary(start_lba, blocks)
    }
    /// Blocks in a Store Superblock unit for this LBA size.
    fn superblock_blocks(self) -> u64 {
        u64::from(4096 / self.lba_bytes)
    }
}

fn base(lba_bytes: u32) -> Atomicity {
    Atomicity {
        lba_bytes,
        awupf_raw: None,
        nawupf_raw: None,
        nabspf_raw: 0,
        nabo_blocks: 0,
    }
}

#[test]
fn case_512_insufficient_atomic_unit_rejects() {
    let a = Atomicity {
        awupf_raw: Some(6), // decoded 7 blocks
        ..base(512)
    };
    assert_eq!(a.effective_blocks(), 7);
    assert_eq!(a.superblock_blocks(), 8);
    assert!(!a.write_is_atomic(0, a.superblock_blocks()));
}

#[test]
fn case_512_eight_block_guarantee_safe_boundary_eligible() {
    let a = Atomicity {
        awupf_raw: Some(7), // decoded 8 blocks
        ..base(512)
    };
    assert!(a.write_is_atomic(0, a.superblock_blocks()));
    assert!(a.write_is_atomic(8, a.superblock_blocks()));
}

#[test]
fn case_512_sufficient_size_but_crosses_boundary_rejects() {
    let a = Atomicity {
        awupf_raw: Some(15), // decoded 16 blocks
        nabspf_raw: 3,       // decoded boundary size 4 blocks
        nabo_blocks: 0,
        ..base(512)
    };
    assert!(a.effective_blocks() >= 8);
    assert!(a.crosses_boundary(0, 8));
    assert!(!a.write_is_atomic(0, 8));
    // A write that fits entirely inside one 4-block region is fine.
    assert!(a.write_is_atomic(0, 4));
}

#[test]
fn case_4096_one_block_guarantee_eligible() {
    let a = Atomicity {
        awupf_raw: Some(0), // decoded 1 block
        ..base(4096)
    };
    assert_eq!(a.superblock_blocks(), 1);
    assert!(a.write_is_atomic(0, 1));
    assert!(a.write_is_atomic(1, 1));
}

#[test]
fn case_namespace_guarantee_overrides_controller() {
    let a = Atomicity {
        awupf_raw: Some(15), // controller would allow 16
        nawupf_raw: Some(0), // namespace guarantees only 1 block
        ..base(512)
    };
    assert_eq!(a.effective_blocks(), 1);
    assert!(!a.write_is_atomic(0, a.superblock_blocks()));
}

#[test]
fn case_namespace_boundary_overrides_when_supported() {
    let a = Atomicity {
        awupf_raw: Some(7),  // decoded 8 blocks
        nawupf_raw: Some(7), // namespace also 8 blocks
        nabspf_raw: 7,       // boundary size 8 blocks, aligned
        nabo_blocks: 0,
        ..base(512)
    };
    assert!(a.write_is_atomic(0, 8));
    // Shift the superblock to a non-aligned start: crosses the boundary at 8.
    assert!(!a.write_is_atomic(4, 8));
}

#[test]
fn case_fallback_to_controller_when_namespace_absent() {
    let a = Atomicity {
        awupf_raw: Some(7),
        nawupf_raw: None,
        ..base(512)
    };
    assert_eq!(a.effective_blocks(), 8);
    assert!(a.write_is_atomic(0, 8));
}
