//! Runtime support for the no_std staticlib.

use core::alloc::{GlobalAlloc, Layout};

#[global_allocator]
static ALLOCATOR: XosAllocator = XosAllocator;

struct XosAllocator;

unsafe extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
}

unsafe impl GlobalAlloc for XosAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { malloc(layout.size()) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe { free(ptr) }
    }
}

#[panic_handler]
fn panic_handler(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
