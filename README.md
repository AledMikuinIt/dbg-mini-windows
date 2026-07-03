# Mini Windows Debugger

A minimal debugger written in C that attaches to a running process, enumerates its loaded modules via PEB/LDR, sets a breakpoint on `user32.dll!MessageBoxA`, and displays CPU register state when the breakpoint is hit.

## How it works

### 1. Attaching to the target process

The debugger takes a process name (e.g. `target.exe`) as a command-line argument, finds its PID using `CreateToolhelp32Snapshot` / `Process32First/Next`, then attaches with `DebugActiveProcess`.

### 2. Enumerating loaded modules via PEB/LDR

Rather than using a high-level API, the debugger manually walks the Process Environment Block (PEB) of the remote process:
- Uses `NtQueryInformationProcess` (via function pointer to `ntdll.NtQueryInformationProcess`) to get the PEB address.
- Reads the remote PEB with `ReadProcessMemory` (only cares about the `Ldr` field).
- Follows the `Ldr` pointer to the Loader Data structure, which contains `InMemoryOrderModuleList`, a doubly-linked list of all loaded modules.
- Walks the list using `LIST_ENTRY.Flink`, reconstructing the full `LDR_DATA_TABLE_ENTRY` for each module using `offsetof` arithmetic, and prints each DLL's name and base address.

This demonstrates direct manipulation of Windows internals rather than relying on abstraction APIs, each structure (`PEB_PARTIAL`, `PEB_LDR_DATA_PARTIAL`, `LDR_DATA_TABLE_ENTRY_PARTIAL`) is hand-crafted to only include the fields needed, since reading remote process memory requires exact structure layouts.

### 3. Setting a breakpoint

On `CREATE_PROCESS_DEBUG_EVENT`, the debugger:
- Resolves `MessageBoxA` in its own process with `GetProcAddress(user32.dll, "MessageBoxA")`.
- Assumes the remote process loads `user32.dll` at the same address (valid for most Windows builds, though ASLR can make this fragile).
- Reads the original first byte at that address (saved in `g_bp.original`).
- Overwrites it with `0xCC` (the INT3 / BREAKPOINT instruction).
- Flushes the instruction cache to ensure the CPU sees the change.

### 4. Handling the breakpoint hit

When the target process hits the INT3:
- A `DEBUG_EVENT` with `ExceptionCode == EXCEPTION_BREAKPOINT` fires.
- The debugger opens a handle to the hitting thread, reads its `CONTEXT` (CPU register state), and decrements `RIP` by 1 (to undo the INT3 advance).
- Prints the register state (RIP, RCX, RDX, R8, R9, the first four arguments on x64).
- Restores the original byte at the breakpoint address.
- Sets the **Trap Flag** (`EFlags |= 0x100`) in the thread's context and resumes with `ContinueDebugEvent`.

### 5. Single-stepping to re-arm the breakpoint

The Trap Flag causes a `SINGLE_STEP` exception after every instruction. When `EXCEPTION_SINGLE_STEP` fires and a single-step is pending:
- The debugger writes the INT3 back to the breakpoint address.
- Clears the pending flag and resumes normal execution.

This two-step dance (remove breakpoint, single-step, replace breakpoint) lets the original code execute once without the INT3 in the way, avoiding an infinite loop.

## Build & run

```bash
gcc -o target.exe target.c
gcc -o dbg.exe dbg.c
```

Launch the target first (it loops calling `MessageBoxA`), then run the debugger:

```bash
.\dbg.exe target.exe
```

Example output:

```
PID: 1234
Debug Attached: 1

################
##### PEB ######
################

PEB: 0x00007ffe0001b000
LDR: 0x00000193beef0140
[MODULE] 0x00007ff6d5080000 C:\Windows\System32\user32.dll
[MODULE] 0x00007ff6d3000000 C:\Windows\System32\kernel32.dll
...

##################
##### DEBUG ######
##################

[EVENT] CREATE PROCESS DEBUG : pid=1234 tid=5678
[BREAKPOINT] Set INT3 at 0x00007ff6d5081450 (original = 0x48)
[BREAKPOINT] INT3 hit at 0x00007ff6d5081450
[INFORMATION]
RIP=0x00007ff6d5081450
RCX=0x0000000000000000
RDX=0x0000000000000000
R8=0x0000000000000000
R9=0x0000000000000000
[BREAKPOINT] Re-armed INT3 at 0x00007ff6d5081450
...
```

## What this demonstrates

- Attaching to a running process and waiting for debug events.
- Manual traversal of Windows internal structures (PEB/LDR) using remote memory reads, rather than relying on debug symbols or APIs.
- Breakpoint handling: replacing code with INT3, restoring original bytes, and using single-step to avoid re-triggering the breakpoint immediately.
- CPU context manipulation: reading and writing registers, setting flags (Trap Flag).
- Cross-architecture support via `#ifdef _M_X64` for x64 vs x86 register differences.

## Known limitations

- **Hardcoded target**: only breaks on `MessageBoxA` in `user32.dll`, generalizing to arbitrary functions would require parsing command-line arguments for address/symbol names.
- **No ASLR awareness**: assumes the target process loads DLLs at the same address as the debugger's own process, which fails if ASLR is enabled on the target or if DLL versions differ.
- **Thread handle leak**: when a breakpoint is hit, the thread handle is opened but never explicitly closed (see comment `// Fermer le Thread !!!` in the code), over many breakpoint hits, this leaks handles.
- **Single-step is inefficient**: a single-step exception fires after *every* instruction following the INT3, even though we only care about re-arming the breakpoint. A more sophisticated approach would use a flag in the thread's context or a separate tracking structure.
- **No multi-threaded breakpoint safety**: if multiple threads hit the breakpoint simultaneously, the re-arming logic could race, one thread might overwrite the breakpoint while another is single-stepping.
- **Assumes matching addresses**: if the target and debugger load DLLs at different addresses (e.g. due to ASLR or different versions), the breakpoint address will be wrong or invalid.

## Next steps

- Parameterize the target function (breakpoint address) rather than hardcoding `MessageBoxA`.
- Properly close thread handles to prevent leaks.
- Use a more efficient single-step mechanism (e.g. setting a flag and only resuming after one step, rather than letting every instruction fire).
- Handle ASLR by reading the target's `InMemoryOrderModuleList` to resolve `MessageBoxA` address dynamically, rather than assuming matching load addresses.
- Support multiple breakpoints and multi-threaded debugging scenarios.
