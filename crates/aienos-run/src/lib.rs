//! aienos-run core: CLI, per-user paths, boot disk writer, console pump and the
//! Backend trait (src/backend). Hypervisor code lives only in the backends.

pub mod backend;
pub mod cli;
pub mod console;
pub mod esp;
pub mod paths;
#[cfg(unix)]
pub mod usernet;
