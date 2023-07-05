/*
 * Copyright 2023 The Android Open Source Project
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

//! Contains the InputVerifier, used to validate a stream of input events.

use crate::ffi::RustPointerProperties;
use crate::input::{DeviceId, MotionAction, MotionFlags};
use log::info;
use std::collections::HashMap;
use std::collections::HashSet;

/// The InputVerifier is used to validate a stream of input events.
pub struct InputVerifier {
    name: String,
    should_log: bool,
    touching_pointer_ids_by_device: HashMap<DeviceId, HashSet<i32>>,
    hovering_pointer_ids_by_device: HashMap<DeviceId, HashSet<i32>>,
}

impl InputVerifier {
    /// Create a new InputVerifier.
    pub fn new(name: &str, should_log: bool) -> Self {
        logger::init(
            logger::Config::default()
                .with_tag_on_device("InputVerifier")
                .with_min_level(log::Level::Trace),
        );
        Self {
            name: name.to_owned(),
            should_log,
            touching_pointer_ids_by_device: HashMap::new(),
            hovering_pointer_ids_by_device: HashMap::new(),
        }
    }

    /// Process a pointer movement event from an InputDevice.
    /// If the event is not valid, we return an error string that describes the issue.
    pub fn process_movement(
        &mut self,
        device_id: DeviceId,
        action: u32,
        pointer_properties: &[RustPointerProperties],
        flags: MotionFlags,
    ) -> Result<(), String> {
        if self.should_log {
            info!(
                "Processing {} for device {:?} ({} pointer{}) on {}",
                MotionAction::from(action).to_string(),
                device_id,
                pointer_properties.len(),
                if pointer_properties.len() == 1 { "" } else { "s" },
                self.name
            );
        }

        match action.into() {
            MotionAction::Down => {
                let it = self
                    .touching_pointer_ids_by_device
                    .entry(device_id)
                    .or_insert_with(HashSet::new);
                let pointer_id = pointer_properties[0].id;
                if it.contains(&pointer_id) {
                    return Err(format!(
                        "{}: Invalid DOWN event - pointers already down for device {:?}: {:?}",
                        self.name, device_id, it
                    ));
                }
                it.insert(pointer_id);
            }
            MotionAction::PointerDown { action_index } => {
                if !self.touching_pointer_ids_by_device.contains_key(&device_id) {
                    return Err(format!(
                        "{}: Received POINTER_DOWN but no pointers are currently down \
                        for device {:?}",
                        self.name, device_id
                    ));
                }
                let it = self.touching_pointer_ids_by_device.get_mut(&device_id).unwrap();
                let pointer_id = pointer_properties[action_index].id;
                if it.contains(&pointer_id) {
                    return Err(format!(
                        "{}: Pointer with id={} not found in the properties",
                        self.name, pointer_id
                    ));
                }
                it.insert(pointer_id);
            }
            MotionAction::Move => {
                if !self.ensure_touching_pointers_match(device_id, pointer_properties) {
                    return Err(format!(
                        "{}: ACTION_MOVE touching pointers don't match",
                        self.name
                    ));
                }
            }
            MotionAction::PointerUp { action_index } => {
                if !self.touching_pointer_ids_by_device.contains_key(&device_id) {
                    return Err(format!(
                        "{}: Received POINTER_UP but no pointers are currently down for device \
                        {:?}",
                        self.name, device_id
                    ));
                }
                let it = self.touching_pointer_ids_by_device.get_mut(&device_id).unwrap();
                let pointer_id = pointer_properties[action_index].id;
                it.remove(&pointer_id);
            }
            MotionAction::Up => {
                if !self.touching_pointer_ids_by_device.contains_key(&device_id) {
                    return Err(format!(
                        "{} Received ACTION_UP but no pointers are currently down for device {:?}",
                        self.name, device_id
                    ));
                }
                let it = self.touching_pointer_ids_by_device.get_mut(&device_id).unwrap();
                if it.len() != 1 {
                    return Err(format!(
                        "{}: Got ACTION_UP, but we have pointers: {:?} for device {:?}",
                        self.name, it, device_id
                    ));
                }
                let pointer_id = pointer_properties[0].id;
                if !it.contains(&pointer_id) {
                    return Err(format!(
                        "{}: Got ACTION_UP, but pointerId {} is not touching. Touching pointers:\
                        {:?} for device {:?}",
                        self.name, pointer_id, it, device_id
                    ));
                }
                self.touching_pointer_ids_by_device.remove(&device_id);
            }
            MotionAction::Cancel => {
                if !flags.contains(MotionFlags::CANCELED) {
                    return Err(format!(
                        "{}: For ACTION_CANCEL, must set FLAG_CANCELED",
                        self.name
                    ));
                }
                if !self.ensure_touching_pointers_match(device_id, pointer_properties) {
                    return Err(format!(
                        "{}: Got ACTION_CANCEL, but the pointers don't match. \
                        Existing pointers: {:?}",
                        self.name, self.touching_pointer_ids_by_device
                    ));
                }
                self.touching_pointer_ids_by_device.remove(&device_id);
            }
            /*
             * The hovering protocol currently supports a single pointer only, because we do not
             * have ACTION_HOVER_POINTER_ENTER or ACTION_HOVER_POINTER_EXIT.
             * Still, we are keeping the infrastructure here pretty general in case that is
             * eventually supported.
             */
            MotionAction::HoverEnter => {
                if self.hovering_pointer_ids_by_device.contains_key(&device_id) {
                    return Err(format!(
                        "{}: Invalid HOVER_ENTER event - pointers already hovering for device {:?}:\
                        {:?}",
                        self.name, device_id, self.hovering_pointer_ids_by_device
                    ));
                }
                let it = self
                    .hovering_pointer_ids_by_device
                    .entry(device_id)
                    .or_insert_with(HashSet::new);
                it.insert(pointer_properties[0].id);
            }
            MotionAction::HoverMove => {
                // For compatibility reasons, we allow HOVER_MOVE without a prior HOVER_ENTER.
                // If there was no prior HOVER_ENTER, just start a new hovering pointer.
                let it = self
                    .hovering_pointer_ids_by_device
                    .entry(device_id)
                    .or_insert_with(HashSet::new);
                it.insert(pointer_properties[0].id);
            }
            MotionAction::HoverExit => {
                if !self.hovering_pointer_ids_by_device.contains_key(&device_id) {
                    return Err(format!(
                        "{}: Invalid HOVER_EXIT event - no pointers are hovering for device {:?}",
                        self.name, device_id
                    ));
                }
                let pointer_id = pointer_properties[0].id;
                let it = self.hovering_pointer_ids_by_device.get_mut(&device_id).unwrap();
                it.remove(&pointer_id);

                if !it.is_empty() {
                    return Err(format!(
                        "{}: Removed hovering pointer {}, but pointers are still\
                               hovering for device {:?}: {:?}",
                        self.name, pointer_id, device_id, it
                    ));
                }
                self.hovering_pointer_ids_by_device.remove(&device_id);
            }
            _ => return Ok(()),
        }
        Ok(())
    }

    /// Notify the verifier that the device has been reset, which will cause the verifier to erase
    /// the current internal state for this device. Subsequent events from this device are expected
    //// to start a new gesture.
    pub fn reset_device(&mut self, device_id: DeviceId) {
        self.touching_pointer_ids_by_device.remove(&device_id);
        self.hovering_pointer_ids_by_device.remove(&device_id);
    }

    fn ensure_touching_pointers_match(
        &self,
        device_id: DeviceId,
        pointer_properties: &[RustPointerProperties],
    ) -> bool {
        let Some(pointers) = self.touching_pointer_ids_by_device.get(&device_id) else {
            return false;
        };

        for pointer_property in pointer_properties.iter() {
            let pointer_id = pointer_property.id;
            if !pointers.contains(&pointer_id) {
                return false;
            }
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use crate::input_verifier::InputVerifier;
    use crate::DeviceId;
    use crate::MotionFlags;
    use crate::RustPointerProperties;
    #[test]
    fn single_pointer_stream() {
        let mut verifier = InputVerifier::new("Test", /*should_log*/ false);
        let pointer_properties = Vec::from([RustPointerProperties { id: 0 }]);
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_DOWN,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_MOVE,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_UP,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
    }

    #[test]
    fn multi_device_stream() {
        let mut verifier = InputVerifier::new("Test", /*should_log*/ false);
        let pointer_properties = Vec::from([RustPointerProperties { id: 0 }]);
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_DOWN,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_MOVE,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(2),
                input_bindgen::AMOTION_EVENT_ACTION_DOWN,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(2),
                input_bindgen::AMOTION_EVENT_ACTION_MOVE,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_UP,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
    }

    #[test]
    fn action_cancel() {
        let mut verifier = InputVerifier::new("Test", /*should_log*/ false);
        let pointer_properties = Vec::from([RustPointerProperties { id: 0 }]);
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_DOWN,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_CANCEL,
                &pointer_properties,
                MotionFlags::CANCELED,
            )
            .is_ok());
    }

    #[test]
    fn invalid_action_cancel() {
        let mut verifier = InputVerifier::new("Test", /*should_log*/ false);
        let pointer_properties = Vec::from([RustPointerProperties { id: 0 }]);
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_DOWN,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_CANCEL,
                &pointer_properties,
                MotionFlags::empty(), // forgot to set FLAG_CANCELED
            )
            .is_err());
    }

    #[test]
    fn invalid_up() {
        let mut verifier = InputVerifier::new("Test", /*should_log*/ false);
        let pointer_properties = Vec::from([RustPointerProperties { id: 0 }]);
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_UP,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_err());
    }

    #[test]
    fn correct_hover_sequence() {
        let mut verifier = InputVerifier::new("Test", /*should_log*/ false);
        let pointer_properties = Vec::from([RustPointerProperties { id: 0 }]);
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_HOVER_ENTER,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());

        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_HOVER_MOVE,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());

        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_HOVER_EXIT,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());

        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_HOVER_ENTER,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());
    }

    #[test]
    fn double_hover_enter() {
        let mut verifier = InputVerifier::new("Test", /*should_log*/ false);
        let pointer_properties = Vec::from([RustPointerProperties { id: 0 }]);
        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_HOVER_ENTER,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_ok());

        assert!(verifier
            .process_movement(
                DeviceId(1),
                input_bindgen::AMOTION_EVENT_ACTION_HOVER_ENTER,
                &pointer_properties,
                MotionFlags::empty(),
            )
            .is_err());
    }
}
