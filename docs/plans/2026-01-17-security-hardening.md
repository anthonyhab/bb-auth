# Noctalia Polkit Security Hardening and Cleanup Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Clean up legacy QML code and restore essential security hardening measures (memory safety, IPC limits, systemd isolation) while maintaining UI compatibility.

**Architecture:**
- Remove abandoned QML integration files.
- Re-introduce memory safety utils (`secureZero`) and use them for sensitive data.
- Integrate auth retry limits into the new multi-session `CPolkitListener` architecture.
- Apply non-breaking systemd hardening and CMake security flags.

**Tech Stack:** C++23, Qt6, Polkit-Qt-1, Systemd.

---

### Task 1: Dead Code Removal

**Files:**
- Delete: `qml/` (directory)
- Delete: `src/QMLIntegration.cpp`
- Delete: `src/QMLIntegration.hpp`

**Step 1: Remove files and directories**
Run: `rm -rf qml/ src/QMLIntegration.cpp src/QMLIntegration.hpp`

**Step 2: Verify they are gone**
Run: `ls src/QMLIntegration.cpp`
Expected: `ls: cannot access 'src/QMLIntegration.cpp': No such file or directory`

**Step 3: Commit**
```bash
git add .
git commit -m "cleanup: remove dead QML integration code"
```

---

### Task 2: Memory Safety (secureZero)

**Files:**
- Modify: `src/core/Agent.cpp`

**Step 1: Add secureZero utility**
Add to `src/core/Agent.cpp` (in anonymous namespace):
```cpp
namespace {
void secureZero(void* ptr, size_t len) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len--) *p++ = 0;
}
}
```

**Step 2: Use secureZero for password responses**
Modify `CAgent::respondToKeyringRequest` to zero the password buffer.

**Step 3: Verify build**
Run: `./build-dev.sh build`
Expected: PASS

**Step 4: Commit**
```bash
git commit -am "security: add secureZero and use it for keyring password buffers"
```

---

### Task 3: IPC Hardening & Auth Retries

**Files:**
- Modify: `src/core/Agent.cpp`
- Modify: `src/core/PolkitListener.hpp`
- Modify: `src/core/PolkitListener.cpp`

**Step 1: Add IPC message size limit**
Modify `CAgent::handleSocketLine` to reject lines > 64KB.

**Step 2: Add retryCount to SessionState**
Modify `src/core/PolkitListener.hpp`:
```cpp
struct SessionState {
    // ...
    int retryCount = 0;
    static constexpr int MAX_AUTH_RETRIES = 3;
};
```

**Step 3: Implement retry logic in PolkitListener**
Update `CPolkitListener::finishAuth` to increment `retryCount` and check against `MAX_AUTH_RETRIES`.

**Step 4: Verify build**
Run: `./build-dev.sh build`
Expected: PASS

**Step 5: Commit**
```bash
git commit -am "security: add IPC message size limits and auth retry logic"
```

---

### Task 4: CMake Security Flags

**Files:**
- Modify: `CMakeLists.txt`

**Step 1: Restore hardening flags**
Add to `CMakeLists.txt`:
```cmake
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Werror=format-security)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_definitions(_FORTIFY_SOURCE=2)
    endif()
    add_link_options(-Wl,-z,relro -Wl,-z,now)
endif()
```

**Step 2: Verify build**
Run: `./build-dev.sh build`
Expected: PASS

**Step 3: Commit**
```bash
git commit -am "build: restore CMake security hardening flags"
```

---

### Task 5: Systemd Service Hardening (Non-Breaking)

**Files:**
- Modify: `assets/noctalia-polkit.service.in`

**Step 1: Re-add safe isolation flags**
Add to `[Service]` section:
```ini
# Security hardening
PrivateTmp=yes
ProtectSystem=strict
NoNewPrivileges=true
CapabilityBoundingSet=
SystemCallFilter=@system-service
SystemCallArchitectures=native
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
MemoryDenyWriteExecute=true
```

**Step 2: Verify build (generates service file)**
Run: `./build-dev.sh build`
Expected: PASS

**Step 3: Commit**
```bash
git commit -am "security: re-add non-breaking systemd isolation flags"
```
