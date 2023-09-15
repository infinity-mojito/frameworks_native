// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.android.graphics.bufferstreamsdemoapp;

import android.os.Bundle;
import android.widget.TextView;
import androidx.appcompat.app.AppCompatActivity;

public class MainActivity extends AppCompatActivity {
    // Used to load the 'bufferstreamsdemoapp' library on application startup.
    static { System.loadLibrary("bufferstreamdemoapp"); }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        RunBufferQueue();
        System.out.println("stringFromJNI: " + stringFromJNI());
    }

    /**
     * A native method that is implemented by the 'bufferstreamsdemoapp' native
     * library, which is packaged with this application.
     */
    public native String stringFromJNI();
    public native void RunBufferQueue();
}