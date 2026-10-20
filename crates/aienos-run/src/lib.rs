//! aienos-run: boot AienOS (Aienux kernel) in a virtual machine on Linux, Windows and
//! macOS. This crate is the shared core (CLI, manifest + verification, image cache,
//! Store disk, console, port-forward table) plus the [`backend::Backend`] seam each host
//! implements. docs/AIENOS_RUN_LINUX.md describes it for users.

pub mod app;
pub mod backend;
pub mod bundle;
pub mod cli;
pub mod console;
pub mod disk;
pub mod fetch;
pub mod manifest;
pub mod net;
pub mod paths;
#[cfg(windows)]
pub mod win;
