//! Thin Objective-C runtime and libdispatch FFI for the Virtualization.framework
//! backend. No crates: `objc_msgSend` is cast to the exact signature of each call
//! (required on arm64), and the two completion handlers Virtualization.framework
//! needs are global blocks built by hand (Block ABI, BLOCK_IS_GLOBAL), so no
//! block runtime copies or frees them.
#![allow(non_upper_case_globals)]

use std::ffi::{c_char, c_void, CStr, CString};

pub type Id = *mut c_void;
pub type Sel = *const c_void;

#[link(name = "objc")]
extern "C" {
    fn objc_getClass(name: *const c_char) -> Id;
    fn sel_registerName(name: *const c_char) -> Sel;
    pub fn objc_msgSend();
    fn objc_autoreleasePoolPush() -> *mut c_void;
    fn objc_autoreleasePoolPop(pool: *mut c_void);
}

#[link(name = "Foundation", kind = "framework")]
extern "C" {}

#[link(name = "Virtualization", kind = "framework")]
extern "C" {}

// libSystem: libdispatch and the block runtime.
extern "C" {
    static _NSConcreteGlobalBlock: [usize; 0];
    fn dispatch_queue_create(label: *const c_char, attr: *const c_void) -> *mut c_void;
    fn dispatch_sync_f(queue: *mut c_void, ctx: *mut c_void, work: extern "C" fn(*mut c_void));
}

pub fn class(name: &str) -> Id {
    let c = CString::new(name).expect("class name");
    // SAFETY: objc_getClass takes a NUL-terminated name and returns nil if absent.
    let k = unsafe { objc_getClass(c.as_ptr()) };
    assert!(!k.is_null(), "Objective-C class {name} not found (macOS too old?)");
    k
}

pub fn sel(name: &str) -> Sel {
    let c = CString::new(name).expect("selector");
    // SAFETY: sel_registerName interns a NUL-terminated selector name.
    unsafe { sel_registerName(c.as_ptr()) }
}

/// `msg!(receiver, "sel:with:", a => T, b => U ; Ret)`: one typed objc_msgSend.
#[macro_export]
macro_rules! msg {
    ($obj:expr, $sel:expr $(, $a:expr => $t:ty)* ; $r:ty) => {{
        let f: unsafe extern "C" fn($crate::backend::objc::Id, $crate::backend::objc::Sel $(, $t)*) -> $r =
            ::std::mem::transmute($crate::backend::objc::objc_msgSend as unsafe extern "C" fn());
        f($obj as $crate::backend::objc::Id, $crate::backend::objc::sel($sel) $(, $a)*)
    }};
}
pub use crate::msg;

/// `[[Class alloc] init]`-style: `new_obj("VZEFIBootLoader")`.
pub fn new_obj(cls: &str) -> Id {
    // SAFETY: +new on an NSObject subclass returns a retained instance.
    unsafe { msg!(class(cls), "new"; Id) }
}

pub fn alloc(cls: &str) -> Id {
    // SAFETY: +alloc returns an uninitialised retained instance to be sent an init.
    unsafe { msg!(class(cls), "alloc"; Id) }
}

pub fn nsstring(s: &str) -> Id {
    let c = CString::new(s).expect("NUL in string");
    // SAFETY: stringWithUTF8String copies the bytes.
    unsafe { msg!(class("NSString"), "stringWithUTF8String:", c.as_ptr() => *const c_char; Id) }
}

pub fn file_url(path: &str) -> Id {
    // SAFETY: fileURLWithPath: takes an NSString.
    unsafe { msg!(class("NSURL"), "fileURLWithPath:", nsstring(path) => Id; Id) }
}

pub fn nsarray(items: &[Id]) -> Id {
    // SAFETY: arrayWithObjects:count: copies `count` object pointers.
    unsafe {
        msg!(class("NSArray"), "arrayWithObjects:count:", items.as_ptr() => *const Id, items.len() => usize; Id)
    }
}

pub fn file_handle(fd: i32) -> Id {
    // SAFETY: initWithFileDescriptor: wraps an open descriptor (not closed on dealloc).
    unsafe { msg!(alloc("NSFileHandle"), "initWithFileDescriptor:", fd => i32; Id) }
}

/// `[obj description]` (or `localizedDescription` for NSError) as a Rust string.
pub fn describe(obj: Id) -> String {
    if obj.is_null() {
        return "nil".into();
    }
    // SAFETY: every NSObject answers description; NSError answers localizedDescription.
    unsafe {
        let is_err: bool = msg!(obj, "isKindOfClass:", class("NSError") => Id; bool);
        let s: Id = if is_err { msg!(obj, "localizedDescription"; Id) } else { msg!(obj, "description"; Id) };
        if s.is_null() {
            return "nil".into();
        }
        let p: *const c_char = msg!(s, "UTF8String"; *const c_char);
        if p.is_null() {
            return "nil".into();
        }
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

pub struct Pool(*mut c_void);
impl Pool {
    pub fn new() -> Self {
        // SAFETY: push/pop are balanced by Drop.
        Pool(unsafe { objc_autoreleasePoolPush() })
    }
}
impl Drop for Pool {
    fn drop(&mut self) {
        // SAFETY: pops the pool pushed in new().
        unsafe { objc_autoreleasePoolPop(self.0) }
    }
}

/// A serial dispatch queue; the VM object must only be touched on it.
pub struct Queue(*mut c_void);
unsafe impl Send for Queue {}
unsafe impl Sync for Queue {}
impl Queue {
    pub fn new(label: &str) -> Self {
        let c = CString::new(label).expect("label");
        // SAFETY: NULL attr = serial queue; the label is copied.
        Queue(unsafe { dispatch_queue_create(c.as_ptr(), std::ptr::null()) })
    }
    pub fn raw(&self) -> Id {
        self.0
    }
    /// Runs `f` on the queue and waits for it.
    pub fn sync<R, F: FnOnce() -> R>(&self, f: F) -> R {
        struct Ctx<F, R> {
            f: Option<F>,
            r: Option<R>,
        }
        extern "C" fn tramp<F: FnOnce() -> R, R>(p: *mut c_void) {
            // SAFETY: p is the &mut Ctx passed below, alive for the dispatch_sync_f call.
            let c = unsafe { &mut *(p as *mut Ctx<F, R>) };
            let _pool = Pool::new();
            c.r = Some((c.f.take().expect("once"))());
        }
        let mut c = Ctx { f: Some(f), r: None };
        // SAFETY: dispatch_sync_f returns only after tramp ran.
        unsafe { dispatch_sync_f(self.0, &mut c as *mut _ as *mut c_void, tramp::<F, R>) };
        c.r.expect("queue work ran")
    }
}

// ---- hand-built global blocks: void (^)(NSError *) ----
#[repr(C)]
struct BlockDesc {
    reserved: usize,
    size: usize,
}
#[repr(C)]
pub struct ErrBlock {
    isa: *const c_void,
    flags: i32,
    reserved: i32,
    invoke: unsafe extern "C" fn(*const ErrBlock, Id),
    desc: *const BlockDesc,
}
unsafe impl Sync for ErrBlock {}
const BLOCK_IS_GLOBAL: i32 = 1 << 28;
static ERR_DESC: BlockDesc = BlockDesc { reserved: 0, size: std::mem::size_of::<ErrBlock>() };

/// One completion slot per handler kind: 0 pending, 1 ok, 2 error (message kept).
pub struct Completion {
    state: std::sync::atomic::AtomicU8,
    msg: std::sync::Mutex<String>,
}
impl Completion {
    pub const fn new() -> Self {
        Completion { state: std::sync::atomic::AtomicU8::new(0), msg: std::sync::Mutex::new(String::new()) }
    }
    pub fn reset(&self) {
        self.state.store(0, std::sync::atomic::Ordering::SeqCst);
    }
    fn set(&self, err: Id) {
        if err.is_null() {
            self.state.store(1, std::sync::atomic::Ordering::SeqCst);
        } else {
            *self.msg.lock().unwrap() = describe(err);
            self.state.store(2, std::sync::atomic::Ordering::SeqCst);
        }
    }
    /// None while pending; Some(Ok) / Some(Err(message)) once called.
    pub fn poll(&self) -> Option<Result<(), String>> {
        match self.state.load(std::sync::atomic::Ordering::SeqCst) {
            0 => None,
            1 => Some(Ok(())),
            _ => Some(Err(self.msg.lock().unwrap().clone())),
        }
    }
}

pub static START_DONE: Completion = Completion::new();
pub static STOP_DONE: Completion = Completion::new();

unsafe extern "C" fn start_invoke(_b: *const ErrBlock, err: Id) {
    START_DONE.set(err);
}
unsafe extern "C" fn stop_invoke(_b: *const ErrBlock, err: Id) {
    STOP_DONE.set(err);
}

pub struct BlockRef(&'static ErrBlock);
impl BlockRef {
    pub fn ptr(&self) -> Id {
        self.0 as *const ErrBlock as Id
    }
}

fn global_isa() -> *const c_void {
    // SAFETY: address of the libSystem global block class symbol.
    unsafe { _NSConcreteGlobalBlock.as_ptr() as *const c_void }
}

pub fn start_block() -> BlockRef {
    static B: std::sync::OnceLock<ErrBlock> = std::sync::OnceLock::new();
    BlockRef(B.get_or_init(|| ErrBlock {
        isa: global_isa(),
        flags: BLOCK_IS_GLOBAL,
        reserved: 0,
        invoke: start_invoke,
        desc: &ERR_DESC,
    }))
}

pub fn stop_block() -> BlockRef {
    static B: std::sync::OnceLock<ErrBlock> = std::sync::OnceLock::new();
    BlockRef(B.get_or_init(|| ErrBlock {
        isa: global_isa(),
        flags: BLOCK_IS_GLOBAL,
        reserved: 0,
        invoke: stop_invoke,
        desc: &ERR_DESC,
    }))
}

unsafe impl Send for ErrBlock {}
