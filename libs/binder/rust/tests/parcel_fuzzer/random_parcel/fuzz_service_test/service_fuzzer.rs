/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#![allow(missing_docs)]
#![no_main]
#[macro_use]
extern crate libfuzzer_sys;

use binder::{self, BinderFeatures, Interface};
use binder_random_parcel_rs::fuzz_service;
use testServiceInterface::aidl::ITestService::{self, BnTestService};

struct TestService;

impl Interface for TestService {}

impl ITestService::ITestService for TestService {
    fn repeatData(&self, token: bool) -> binder::Result<bool> {
        Ok(token)
    }
}

fuzz_target!(|data: &[u8]| {
    let service = BnTestService::new_binder(TestService, BinderFeatures::default());
    fuzz_service(&mut service.as_binder(), data);
});
