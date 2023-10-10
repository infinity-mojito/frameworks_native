// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Pleasant Rust bindings for libnativewindow, including AHardwareBuffer

extern crate nativewindow_bindgen as ffi;

pub use ffi::{AHardwareBuffer_Format, AHardwareBuffer_UsageFlags};

use std::os::raw::c_void;
use std::ptr;

/// Wrapper around an opaque C AHardwareBuffer.
pub struct AHardwareBuffer(*mut ffi::AHardwareBuffer);

impl AHardwareBuffer {
    /// Test whether the given format and usage flag combination is allocatable.  If this function
    /// returns true, it means that a buffer with the given description can be allocated on this
    /// implementation, unless resource exhaustion occurs. If this function returns false, it means
    /// that the allocation of the given description will never succeed.
    ///
    /// Available since API 29
    pub fn is_supported(
        width: u32,
        height: u32,
        layers: u32,
        format: AHardwareBuffer_Format::Type,
        usage: AHardwareBuffer_UsageFlags,
        stride: u32,
    ) -> bool {
        let buffer_desc = ffi::AHardwareBuffer_Desc {
            width,
            height,
            layers,
            format,
            usage: usage.0,
            stride,
            rfu0: 0,
            rfu1: 0,
        };
        // SAFETY: *buffer_desc will never be null.
        let status = unsafe { ffi::AHardwareBuffer_isSupported(&buffer_desc) };

        status == 1
    }

    /// Allocates a buffer that matches the passed AHardwareBuffer_Desc. If allocation succeeds, the
    /// buffer can be used according to the usage flags specified in its description. If a buffer is
    /// used in ways not compatible with its usage flags, the results are undefined and may include
    /// program termination.
    ///
    /// Available since API level 26.
    #[inline]
    pub fn new(
        width: u32,
        height: u32,
        layers: u32,
        format: AHardwareBuffer_Format::Type,
        usage: AHardwareBuffer_UsageFlags,
    ) -> Option<Self> {
        let buffer_desc = ffi::AHardwareBuffer_Desc {
            width,
            height,
            layers,
            format,
            usage: usage.0,
            stride: 0,
            rfu0: 0,
            rfu1: 0,
        };
        let mut buffer = ptr::null_mut();
        // SAFETY: The returned pointer is valid until we drop/deallocate it. The function may fail
        // and return a status, but we check it later.
        let status = unsafe { ffi::AHardwareBuffer_allocate(&buffer_desc, &mut buffer) };

        if status == 0 {
            Some(Self(buffer))
        } else {
            None
        }
    }

    /// Adopts the raw pointer and wraps it in a Rust AHardwareBuffer.
    ///
    /// # Errors
    ///
    /// Will panic if buffer_ptr is null.
    ///
    /// # Safety
    ///
    /// This function adopts the pointer but does NOT increment the refcount on the buffer. If the
    /// caller uses the pointer after the created object is dropped it will cause a memory leak.
    pub unsafe fn take_from_raw(buffer_ptr: *mut c_void) -> Self {
        assert!(!buffer_ptr.is_null());
        Self(buffer_ptr as *mut ffi::AHardwareBuffer)
    }

    /// Get the system wide unique id for an AHardwareBuffer. This function may panic in extreme
    /// and undocumented circumstances.
    ///
    /// Available since API level 31.
    pub fn id(&self) -> u64 {
        let mut out_id = 0;
        // SAFETY: Neither pointers can be null.
        let status = unsafe { ffi::AHardwareBuffer_getId(self.0, &mut out_id) };
        assert_eq!(status, 0, "id() failed for AHardwareBuffer with error code: {status}");

        out_id
    }

    /// Get the width of this buffer
    pub fn width(&self) -> u32 {
        self.description().width
    }

    /// Get the height of this buffer
    pub fn height(&self) -> u32 {
        self.description().height
    }

    /// Get the number of layers of this buffer
    pub fn layers(&self) -> u32 {
        self.description().layers
    }

    /// Get the format of this buffer
    pub fn format(&self) -> AHardwareBuffer_Format::Type {
        self.description().format
    }

    /// Get the usage bitvector of this buffer
    pub fn usage(&self) -> AHardwareBuffer_UsageFlags {
        AHardwareBuffer_UsageFlags(self.description().usage)
    }

    /// Get the stride of this buffer
    pub fn stride(&self) -> u32 {
        self.description().stride
    }

    fn description(&self) -> ffi::AHardwareBuffer_Desc {
        let mut buffer_desc = ffi::AHardwareBuffer_Desc {
            width: 0,
            height: 0,
            layers: 0,
            format: 0,
            usage: 0,
            stride: 0,
            rfu0: 0,
            rfu1: 0,
        };
        // SAFETY: neither the buffer nor AHardwareBuffer_Desc pointers will be null.
        unsafe { ffi::AHardwareBuffer_describe(self.0, &mut buffer_desc) };
        buffer_desc
    }
}

impl Drop for AHardwareBuffer {
    fn drop(&mut self) {
        // SAFETY: self.0 will never be null. AHardwareBuffers allocated from within Rust will have
        // a refcount of one, and there is a safety warning on taking an AHardwareBuffer from a raw
        // pointer requiring callers to ensure the refcount is managed appropriately.
        unsafe { ffi::AHardwareBuffer_release(self.0) }
    }
}

#[cfg(test)]
mod ahardwarebuffer_tests {
    use super::*;

    #[test]
    fn create_valid_buffer_returns_ok() {
        let buffer = AHardwareBuffer::new(
            512,
            512,
            1,
            AHardwareBuffer_Format::AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        );
        assert!(buffer.is_some());
    }

    #[test]
    fn create_invalid_buffer_returns_err() {
        let buffer = AHardwareBuffer::new(512, 512, 1, 0, AHardwareBuffer_UsageFlags(0));
        assert!(buffer.is_none());
    }

    #[test]
    #[should_panic]
    fn take_from_raw_panics_on_null() {
        // SAFETY: Passing a null pointer is safe, it should just panic.
        unsafe { AHardwareBuffer::take_from_raw(ptr::null_mut()) };
    }

    #[test]
    fn take_from_raw_allows_getters() {
        let buffer_desc = ffi::AHardwareBuffer_Desc {
            width: 1024,
            height: 512,
            layers: 1,
            format: AHardwareBuffer_Format::AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            usage: AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN.0,
            stride: 0,
            rfu0: 0,
            rfu1: 0,
        };
        let mut raw_buffer_ptr = ptr::null_mut();

        // SAFETY: The pointers are valid because they come from references, and
        // `AHardwareBuffer_allocate` doesn't retain them after it returns.
        let status = unsafe { ffi::AHardwareBuffer_allocate(&buffer_desc, &mut raw_buffer_ptr) };
        assert_eq!(status, 0);

        // SAFETY: The pointer must be valid because it was just allocated successfully, and we
        // don't use it after calling this.
        let buffer = unsafe { AHardwareBuffer::take_from_raw(raw_buffer_ptr as *mut c_void) };
        assert_eq!(buffer.width(), 1024);
    }

    #[test]
    fn basic_getters() {
        let buffer = AHardwareBuffer::new(
            1024,
            512,
            1,
            AHardwareBuffer_Format::AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        )
        .expect("Buffer with some basic parameters was not created successfully");

        assert_eq!(buffer.width(), 1024);
        assert_eq!(buffer.height(), 512);
        assert_eq!(buffer.layers(), 1);
        assert_eq!(buffer.format(), AHardwareBuffer_Format::AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        assert_eq!(
            buffer.usage(),
            AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
        );
    }

    #[test]
    fn id_getter() {
        let buffer = AHardwareBuffer::new(
            1024,
            512,
            1,
            AHardwareBuffer_Format::AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        )
        .expect("Buffer with some basic parameters was not created successfully");

        assert_ne!(0, buffer.id());
    }
}
