//! aienos-run: boot AienOS (Aienux kernel) in a virtual machine on Linux, Windows and
//! macOS. This crate is the shared core (CLI, manifest + verification, image cache,
//! Store disk, console, port-forward table) plus the [`backend::Backend`] seam each host
//! implements. docs/AIENOS_RUN_LINUX.md describes it for users.

pub mod app;
pub mod backend;
pub mod cli;
pub mod console;
pub mod devices;
pub mod disk;
pub mod esp;
pub mod fetch;
pub mod manifest;
pub mod net;
pub mod paths;
#[cfg(unix)]
pub mod usernet;
