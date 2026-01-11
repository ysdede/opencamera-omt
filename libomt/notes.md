"Rewrote entire OMT Sender in pure C++ (libomt_android.cpp)" "Native C++ OMT Sender: Complete reimplementation"

And regarding libvmx:

"VMX Integration: Embedded into libomt.so"

So libomt effectively became our custom C++ port/implementation for Android because the original .NET Native AOT approach failed.

📋 Plan
Extract our Custom Port: Creating a new repo for our custom Android C++ implementation (which lives in libomt/stub and the JNI bridge).
handle Submodules:
libomtnet: Modified (OMTDiscovery.cs changes). Needs a fork.
libvmx: Official repo, but we added build scripts. Needs a fork.
libomt: Official repo, but we added stub/libomt_android.cpp. Needs a fork OR we just keep our custom code in our main project.
Since libomt_android.cpp is our code, we should probably keep it in the main omtandroid repo or a dedicated libomt-android-cpp repo, rather than trying to upstream it to the C# libomt repo.

Action: I will help you organize this.

First, let me confirm the exact files for the "C++ Port" you mentioned. I believe it's libomt/stub/libomt_android.cpp and app/src/main/cpp/*.

Shall I proceed with creating a separate folder/repo instructions for the Android C++ Port?

Analyzed
OMT_DEVELOPMENT_LOGBOOK.md#L800-900
I see. To properly separate the custom C++ OMT port, we should extract 
libomt/stub/libomt_android.cpp
 and app/src/main/cpp/* into a new repository, let's call it libomt-cpp or libomt-android.

But first, since you want to fork the original repos (libomt, libomtnet, libvmx) and apply our changes:

libomt:
Original: C# wrapper (?)
Our Change: Added stub/libomt_android.cpp (The complete C++ implementation).
Strategy: Fork libomt, checkout a new branch (e.g., android-cpp-port), and commit stub/libomt_android.cpp there.
libomtnet:
Our Change: Modified 
src/OMTDiscovery.cs
 (Changes not yet fully confirmed in detail, but likely for the 
RegisterAddress
 visibility or similar).
Strategy: Fork libomtnet, apply the C# changes.
libvmx:
Our Change: Added VMXCodec.cpp compilation support for Android (via our build script or CMake). The file 
libvmx/src/vmxcodec_arm.cpp
 was mentioned.
Strategy: Fork libvmx.
However, 
libomt/stub/libomt_android.cpp
 is a replacement for the C# library, not really a "stub" or "patch". It's a completely different implementation of the protocol in C++.

Recommended Action: Create a New Repository named libomt-cpp (or libomt-android-cpp) which contains:

libomt_android.cpp (renamed to omt_sender.cpp or similar)
libomt.h
 (The header we defined)
CMakeLists.txt (to build it as a standalone lib)
Then, 
omtandroid
 (this repo) would consume libomt-cpp as a submodule, instead of having the source code buried in libomt/stub.

Let's stick to your request: "create a separate repo for it".

I will create a folder libomt-cpp-port in the current directory, copy the relevant files there, and git init it. This will be the "separate repo".

Step 1: Create libomt-cpp-port
I'll copy:

libomt/stub/libomt_android.cpp -> libomt-cpp-port/src/omt_sender.cpp
app/src/main/cpp/libomt.h -> libomt-cpp-port/include/libomt.h
app/src/main/cpp/OMTBridge.cpp -> (Optional, this is Android JNI specific, usually stays in App)
libvmx? No, that's a dependency.
Actually, libomt_android.cpp depends on libvmx.