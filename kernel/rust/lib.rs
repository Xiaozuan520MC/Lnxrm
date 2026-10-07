//! lnxrm Rust support crate.
//!
//! Freestanding (`no_std`), panic-abort, linked as a static library into the
//! kernel. Provides the UART console driver and SMP primitives.

#![no_std]

pub mod uart;
pub mod smp;

use core::panic::PanicInfo;

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    uart::write_str("\n[rust PANIC] ");
    if let Some(loc) = info.location() {
        let mut buf = [0u8; 64];
        let msg = format_into(&mut buf, loc.file(), loc.line() as u64);
        uart::write_str(msg);
    }
    loop {
        unsafe { core::arch::asm!("cli; hlt") }
    }
}

/// tiny formatter: "file:line" into a fixed buffer (no alloc, no fmt machinery)
fn format_into<'a>(buf: &'a mut [u8], file: &str, line: u64) -> &'a str {
    let mut i = 0;
    for &b in file.as_bytes() {
        if i + 1 >= buf.len() {
            break;
        }
        buf[i] = b;
        i += 1;
    }
    if i < buf.len() - 1 && i + 12 < buf.len() {
        buf[i] = b':';
        i += 1;
        let mut n = line;
        if n == 0 {
            buf[i] = b'0';
            i += 1;
        }
        let start = i;
        while n > 0 && i < buf.len() - 1 {
            let d = (n % 10) as u8;
            // shift right to make room (digits are few)
            for j in (start..i).rev() {
                buf[j + 1] = buf[j];
            }
            buf[start] = b'0' + d;
            i += 1;
            n /= 10;
        }
    }
    core::str::from_utf8(&buf[..i]).unwrap_or("?")
}
