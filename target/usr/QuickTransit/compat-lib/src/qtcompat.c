/*
 * Host-side fixes for running the 2005 QuickTransit binary on a current glibc.
 * Linked into the shim QuickTransit loads as "libunwind-ia64.so", so these
 * definitions take precedence over libc's for QuickTransit itself only.
 *
 * Build with -O0: the point is the instruction forms the compiler emits.
 *
 * strncat: IRIX programs may pass NULL paths to syscalls (4Dwm does
 * stat(NULL)).  QuickTransit's Fuse::createSubjectAbsolutePath() then calls
 * strncat(cwd, NULL, n).  Guest page 0 is backed by an empty file, so the read
 * raises SIGBUS and QuickTransit's handler decodes the faulting load and
 * emulates it (returning 0).  The handler only knows plain "ld1 rX=[rY]";
 * glibc 2.44's strncat goes through memcpy's post-increment loads
 * ("ld1 r23=[r21],1"), which it can't decode, so it aborts with
 * "IA64Signalhandler::classify faulty instruction: Unknown instruction".
 * A simple byte loop keeps the loads in a form the handler understands.
 */
#include <stddef.h>

char *strncat(char *dst, const char *src, size_t n)
{
    size_t d = 0, i;

    while (dst[d] != '\0')
        d++;
    for (i = 0; i < n; i++) {
        char c = src[i];
        if (c == '\0')
            break;
        dst[d + i] = c;
    }
    dst[d + i] = '\0';
    return dst;
}

/*
 * Mesa: some legacy GL paths (e.g. tex_cube's) go through gallium's "draw"
 * module, which by default JIT-compiles with LLVM.  LLVM has no JIT for ia64
 * ("Target has no JIT support") and the process dies.  Use the C draw path.
 * Runs before QuickTransit's main(), affects only QuickTransit processes, and
 * leaves a user-set value alone.
 */
#include <stdlib.h>

__attribute__((constructor))
static void qtcompat_env(void)
{
    setenv("DRAW_USE_LLVM", "0", 0);
}
