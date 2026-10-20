//! The few Win32 console calls aienos-run needs, declared by hand (kernel32 is linked by
//! every Windows target; no outside crate). Signatures from the Windows SDK `consoleapi.h`,
//! `wincon.h`, `processenv.h`.

#![allow(non_snake_case, clippy::upper_case_acronyms)]

use std::ffi::c_void;

pub type HANDLE = *mut c_void;
pub type BOOL = i32;
pub type DWORD = u32;

pub const STD_INPUT_HANDLE: DWORD = -10i32 as DWORD;
pub const STD_OUTPUT_HANDLE: DWORD = -11i32 as DWORD;

// Input modes (wincon.h).
pub const ENABLE_PROCESSED_INPUT: DWORD = 0x0001;
pub const ENABLE_LINE_INPUT: DWORD = 0x0002;
pub const ENABLE_ECHO_INPUT: DWORD = 0x0004;
pub const ENABLE_VIRTUAL_TERMINAL_INPUT: DWORD = 0x0200;
// Output modes.
pub const ENABLE_PROCESSED_OUTPUT: DWORD = 0x0001;
pub const ENABLE_VIRTUAL_TERMINAL_PROCESSING: DWORD = 0x0004;

// Control events (wincon.h).
pub const CTRL_C_EVENT: DWORD = 0;
pub const CTRL_BREAK_EVENT: DWORD = 1;
pub const CTRL_CLOSE_EVENT: DWORD = 2;

pub const CP_UTF8: u32 = 65001;

pub type HandlerRoutine = unsafe extern "system" fn(DWORD) -> BOOL;

#[link(name = "kernel32")]
extern "system" {
    pub fn GetStdHandle(nStdHandle: DWORD) -> HANDLE;
    pub fn GetConsoleMode(hConsoleHandle: HANDLE, lpMode: *mut DWORD) -> BOOL;
    pub fn SetConsoleMode(hConsoleHandle: HANDLE, dwMode: DWORD) -> BOOL;
    pub fn SetConsoleCtrlHandler(HandlerRoutine: Option<HandlerRoutine>, Add: BOOL) -> BOOL;
    pub fn SetConsoleTitleW(lpConsoleTitle: *const u16) -> BOOL;
    pub fn GetConsoleProcessList(lpdwProcessList: *mut DWORD, dwProcessCount: DWORD) -> DWORD;
    pub fn SetConsoleOutputCP(wCodePageID: u32) -> BOOL;
    pub fn SetConsoleCP(wCodePageID: u32) -> BOOL;
}

/// UTF-16, NUL-terminated.
pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// The console mode of a standard handle, or None when it is not a console.
pub fn console_mode(which: DWORD) -> Option<(HANDLE, DWORD)> {
    unsafe {
        let h = GetStdHandle(which);
        if h.is_null() || h as isize == -1 {
            return None;
        }
        let mut m: DWORD = 0;
        if GetConsoleMode(h, &mut m) == 0 {
            return None;
        }
        Some((h, m))
    }
}

/// True when this process is the only one attached to its console: it was started by a
/// double click, so the window closes the moment aienos-run exits.
pub fn owns_console_window() -> bool {
    let mut ids = [0u32; 2];
    let n = unsafe { GetConsoleProcessList(ids.as_mut_ptr(), ids.len() as DWORD) };
    n == 1
}
