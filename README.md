# OpenEra

**OpenEra** is an experimental, open-source translation layer and runtime environment designed to run Xbox One and Xbox Series X|S titles (**Era** / **GameCore**) natively on desktop Windows.

Because modern Xbox consoles run a customized Windows NT kernel and Direct3D 12 runtime under a specialized hypervisor, OpenEra focuses on API translation, syscall shimming, and hardware abstraction rather than full x86-64 CPU emulation.

> **Disclaimer:** OpenEra is strictly an educational and interoperability research project. It does not include copyrighted Microsoft binaries, encryption keys, or game assets. Users must supply their own legally obtained dumps.

---

## Architecture Overview

Modern Xbox titles target **Era** (Xbox One) or **GameCore** (Xbox Series X|S) rather than standard Win32. OpenEra bridges those environment differences to standard Windows APIs:

* **Kernel & Syscall Shim:** Translates Xbox-specific NT kernel calls, memory allocation flags, and threading primitives to standard Windows NT equivalents.
* **Graphics (D3D12.x -> D3D12):** Maps Xbox Direct3D 12 extensions, custom command list formats, and ESRAM/memory aliasing behaviors onto standard desktop Direct3D 12.
* **Audio & Storage:** Redirects XMA2 hardware audio decoding and Xbox virtual filesystem (XVD/XVC) package mounts to host equivalents.
* **Input & System Services:** Maps Xbox Gaming Runtime Services (XGR) and controller protocols to Windows Gaming.Input / XInput.

---

## Project Status

OpenEra is currently in **early development**.

- [ ] XVD/XVC container parser and unencrypted package loader
- [ ] Basic PE/DLL loader and import resolver
- [ ] Era / GameCore kernel thunk layer (In Progress)
- [ ] Direct3D 12.x command buffer translation
- [ ] Audio & controller input subsystem
- [ ] First commercial title booting to menu

---

## Building from Source

### Prerequisites
* **OS:** Windows 10 (22H2+) or Windows 11 (x64)
* **Compiler:** Visual Studio 2022 (MSVC v143+) or Clang-CL
* **Build System:** CMake 3.25+ and Ninja
* **SDK:** Windows 11 SDK (10.0.22621.0 or newer)

### Build Instructions

```bash
git clone https://github.com/PlutonusDev/OpenEra.git
cd OpenEra

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
