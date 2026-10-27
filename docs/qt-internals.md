# QuickTransit internals: extension points

Notes from static analysis of QuickTransit 1.2.1-004
(`/usr/QuickTransit/bin/QuickTransit`, vendor md5 `d6ece54e…`). QuickTransit
is closed source, but the binary is not stripped, and it was linked so that
its internal C++ symbols are in the dynamic symbol table (`nm -D` lists
16,000+ symbols). A native ia64 shared library loaded into the process can
therefore call and override its internals by name.

Addresses below are for that build. The executable is `ET_EXEC` (not PIE), so
they're fixed. Use an ia64 `objdump` to disassemble (a cross binutils works;
the host's multi-arch `objdump` usually lacks ia64).

## Syscall dispatch

`M_ISyscallHandler::syscallTable`
(`_ZN17M_ISyscallHandler12syscallTableE`, in the dynamic symbol table) is at
`0x6000000000070a50` in writable `.data`, size `0xef0`:

- 239 entries of 16 bytes: an ia64 function-descriptor pointer, then a zero
  word. Index = IRIX syscall number − 1000 (`SYS_syscall` = 1000,
  `SYS_exit` = 1001, ...).
- The file holds zeros; the dynamic loader fills each slot from an
  `R_IA64_FPTR64LSB` relocation. Every slot is set before any library
  constructor runs.
- Handler signature: `M_ISyscallHandler::IRIX_xxx(ThreadContext*, IsoBlock*,
  int, int)` (plus `this`).
- 23 slots point at `IRIX_invalid` (numbers IRIX doesn't use) and 9 at
  `IRIX_unsupported`: `attr_getf` … `attr_multif` (1192–1200). Every other
  IRIX syscall has a handler.

The gaps that matter in practice are **subcalls inside existing handlers**
(see the list below). Fixing those means wrapping `IRIX_syssgi` or
`IRIX_ioctl`, or patching a case, not filling an empty slot.

### Known gaps

Found so far, with what hits each one and where it stands. Addresses are
in `IRIX_syssgi` (`0x40000000006159c0`), which dispatches subcalls with a
compare tree, plus a table for `SGI_SYSCONF` names.

| Call | QuickTransit behaviour | Hit by | Status |
|---|---|---|---|
| `syssgi(SGI_SETGROUPS = 40)` (`setgroups()`) | logs "SGI_SETGROUPS ignored", returns -1 | `fam` (every client) | patched (`--extras`) to return 0 (no-op). A real implementation would byte-swap the gid list and call Linux `setgroups`. |
| `syssgi(SGI_SATWRITE = 75)` and other unknown subcalls | "Unsupported subcall", process exits | `fam` (`satwrite()`) | patched (`--extras`): logged, returns 0 |
| `sysconf(_SC_AUDIT 26, _SC_INF 27)` | "Unsupported SGI_SYSCONF request", exits | `fam` | patched (`--extras`): return 0 |
| `sysconf(_SC_CHILD_MAX)`, `_SC_OPEN_MAX`, `_SC_NGROUPS_MAX`, `_SC_ARG_MAX` | fixed constants 6, 16, 8, 131072 (POSIX minimums), regardless of host limits | IRIX `sh` ("too many jobs" with more than five background jobs) | CHILD_MAX → 1034, OPEN_MAX → 1024 patched (`--extras`). NGROUPS_MAX and ARG_MAX left (IRIX: 16, 262144). |
| `ioctl(fd, 't' 97)` | "'t' subcall unsupported: 97" | `tar cf file` / `tar xf file` | open; use pipes |
| `/dev/imon` (IRIX inode monitor) | not present | `fam` | none needed: fam polls |
| `attr_*` (1192–1200) | `IRIX_unsupported` | extended attributes | open, nothing seen using them |

The guest-side workaround for missing GL/X behaviour (as opposed to
syscalls) is a preload library; see [irix-desktop.md](irix-desktop.md) and
[iris-gl.md](iris-gl.md). Separately, QuickTransit's postinstall replaces
the programs in `scripts/unsupported-IRIX-binary-list` with a "not
currently supported" script; `install-extras.sh` puts back
`/usr/lib/desktop/waitmap`, which works.

### Adding or wrapping a syscall without patching code

Because the table is writable and exported, a native library can, in its
constructor:

1. save the current descriptor pointer from the slot,
2. store a pointer to its own handler (on ia64 a C function pointer *is* a
   descriptor pointer),
3. handle the cases it knows and call the saved original for the rest.

Ways to load such a library into QuickTransit:
- **Easiest:** add it to the shim the kit already builds. QuickTransit
  already loads `libunwind-ia64.so`, which the kit replaces with its own shim
  (`qtcompat.c`). A constructor there would do.
- `LD_PRELOAD` works for quick tests, but it's ignored for launches through
  `setuid_irixrun` and it's inherited by every child.

Not yet verified: that `M_ISyscallHandler::handle()` indexes this static
table directly, rather than a copy made at startup.

### Reading arguments and returning results

From `IRIX_umask`:
- A handler loads `this+8`, a `SubjectConventions*`. Arguments come from a
  virtual call through vtable slot `+64`, called as
  `(conventions, IsoBlock*, index, …)`.
- The result and error flag are set through vtable slot `+16`.
- Host syscalls are made through helpers like `ia64SC_1arg(nr, …)`, which use
  Linux ia64 syscall numbers (`umask` = 1067).
- Errors are converted with the exported
  `mangleLinuxToIRIXErrno(unsigned long long)`.

These slot meanings come from one function; confirm them on a second handler
before relying on them.

Guest pointers are passed to host syscalls unchanged: `IRIX_chdir` hands the
guest path pointer straight to the host. Guest memory is identity-mapped in
the low 4 GB. **Guest data is big-endian.** Strings are fine as they are, but
integers inside structs and arrays (for example a `gid_t` list) must be
byte-swapped.

## Native library binding ("SCall")

This is how QuickTransit runs IRIX `libmpi`/`libsma` calls, and its own GL
and X11 hooks, as native ia64 code. It works for third-party libraries too,
without patching QuickTransit: the kit's IRIX audio uses it
([irix-audio.md](irix-audio.md), sources in `extras/src/irixaudio/`).
Verified 2026-10-27 with a probe: signed and unsigned ints, a pointer and a
negative return value all came through correctly. A blocking native call in
one IRIX thread doesn't stall the program's other threads.

The IRIX-side stub library contains no real MIPS code for these functions.
Each one is a marker block that the translator
(`MIPS4IRConstructor::SCall_marker`, `0x40000000003e1390`) replaces with a
native call. There are two kinds:

    library declaration              function
    7c000000                         7c000002          marker (unused MIPS opcode)
    02000000                         02000000          option (same in o32, n32, n64)
    "libRCMPI.so\0" padded           "RC_MPI_Send\0" padded
    7c000000                         00014030 ...      one word per argument
    03e00008 00000000  jr ra; nop    01008050          return value
                                     7c000002
                                     03e00008 00000000 jr ra; nop

**Library declaration.** When the declaration is translated, which happens
the first time it runs, `SCall_marker` looks at the name:
- `INBUILT_<version>` binds to functions inside the QuickTransit binary
  (the GL and X11 hooks; the version is checked against
  `RCGL_LibraryVersion`).
- Anything else is `dlopen`ed as `"%s/%s"`, with
  `FuseConfig::fuTargetNBLibraryPath` (config option
  `FU_TARGETNBLIBRARY_PATH`, default `/usr/QuickTransit/lib/ia64`) and
  `RTLD_LAZY|RTLD_GLOBAL`. The handle goes onto a list. `libRCMPI` and
  `libRCSMA` get extra special-casing, but any name works.

Function symbols are then looked up in every library on that list ("Cannot
find symbol '%s' in any library to native bind to!"). So the stub library
has to run its declaration before any function stub is first called:
`X11Hook_initLibrary` does that, and the kit's libaudio calls its
declaration stub once before its first native call.

**Argument and return words.** An argument word is
`MIPS register number << 14 | type`:

| Register field | Source |
|---|---|
| 4–11 (`0x10000`–`0x2c000`) | integer argument registers a0–a7 |
| 13, 14, ... (`0x34000`, ...) | FP argument registers (n32 passes FP args by position, from `$f12`) |
| 29 (`0x74000`) | stack (`$sp`), in order |

| Type | Meaning |
|---|---|
| `0x30` | signed int |
| `0x50` | unsigned int |
| `0x90` | pointer (IRIX pointers are passed as they are: memory is identity-mapped) |
| `0x70` / `0x80` | float / double |

QuickTransit's own stubs mostly start at a1 (`0x14xxx`) and leave a0 out
(`RC_XOpenDisplay(char*)` is `00014090 01008050`, and the native function
takes one argument). `RC_MPI_Symbol_setup` starts at a0 (`00010090`).
Either works.

The return word is `01008050` (v0) in 2,769 of the 2,800 stubs. The others
are `01030070`/`01030080` (float/double in `$f0`, SHMEM) and a few odd ones
in o32 `libX11_hook`. The native `int` result arrives in v0 sign-extended.
The kit declares its stubs as returning `long long` in C and truncates, so
it doesn't depend on that.

Writing stubs with the GNU assembler (n32, from the SGUG binutils):

    .abicalls
    .set noreorder
    qta_libdecl:  .word 0x7c000000, 0x02000000
                  .asciz "libRCAudio.so"
                  .align 2
                  .word 0x7c000000
                  jr $31
                  nop
    qta_write:    .word 0x7c000002, 0x02000000
                  .asciz "RC_qta_write"
                  .align 2
                  .word 0x10030, 0x14090, 0x18030    # int, pointer, int
                  .word 0x01008050, 0x7c000002
                  jr $31
                  nop

Mark the stubs `.hidden` so the library doesn't export them. On the
native side, keep host pointers away from the 32-bit IRIX code: hand out
small integer handles instead.

Not yet worked out: the stack-argument encoding in detail (the low nibble
is 5 in `0x74035`/`0x74055`/`0x74095`), and calls back from native code
into IRIX code.

This mechanism could also carry:
- an IRIS GL implementation (see [iris-gl.md](iris-gl.md)),
- any other IRIX library whose work is better done natively.

## IRIX dynamic linker preload list

On every exec, QuickTransit sets the guest's `_RLD_LIST`, `_RLDN32_LIST` and
`_RLD64_LIST` to its X11/Xi hook libraries, overwriting any value the
environment had. The values come from config options `FU_RLD_LIST`,
`FU_RLDN32_LIST`, `FU_RLDN64_LIST`. Setting these as environment variables
has no effect; they have to be in a config file.

The config file is `/usr/QuickTransit/etc/QuickTransit.conf`
(`OPTION=value` lines). To use a private copy for one run, set the
`DYNAMITE_CONFIG_FILE` environment variable to its path. For example, to
preload an IRIX library ahead of the hooks (keep the hooks and `DEFAULT` in
the list):

    cp /usr/QuickTransit/etc/QuickTransit.conf /root/qt-test.conf
    echo 'FU_RLDN32_LIST=/root/mylib.so:/usr/QuickTransit/lib32/libX11_hook.so:/usr/QuickTransit/lib32/libXi_hook.so:DEFAULT' >> /root/qt-test.conf
    DYNAMITE_CONFIG_FILE=/root/qt-test.conf /irix/usr/bin/someprogram

IRIX `rld` has no `RTLD_NEXT`. An interposer gets the real function with
`dlopen("libGL.so", RTLD_LAZY)` plus `dlsym`.

Other option names in the binary that look useful but haven't been tried:
`FU_DUMP_STRACE`, `FU_DUMP_STRACE_FOR`, `FU_DUMP_STRACE_END` (syscall
tracing), `TRACE_BLOCKS`, `DEBUGLOG`, `FU_DISABLE_LOGGING`, and
`IRIX_GL*` / `FU_GL*_STRING` (strings reported to GL programs).

## ELF loader

`ELFFileLoader` maps segments with `mmap(PROT_EXEC)` from the file, so IRIX
executables can't run from a `noexec` filesystem. T2 mounts `/tmp` that way.
The failure is `ELFFileLoader: Failed to simple-mmap section!`; `strace`
shows `mmap2(…, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_FIXED, …) = -1 EPERM`.
