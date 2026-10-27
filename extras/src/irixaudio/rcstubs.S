/*
 * QuickTransit "SCall" stubs for libaudio.  IRIX n32 side.
 *
 * Each function here is not MIPS code: it is a marker block that
 * QuickTransit's translator (MIPS4IRConstructor::SCall_marker) replaces
 * with a direct native call.  Layout copied from QuickTransit's own stubs
 * (libX11_hook.so, libmpi.so):
 *
 *   library declaration   7c000000 02000000 "<lib>\0" 7c000000; jr ra; nop
 *   function              7c000002 02000000 "<sym>\0" <args> <ret> 7c000002;
 *                         jr ra; nop
 *
 * qta_libdecl() must run before any function stub: translating it dlopens
 * <FU_TARGETNBLIBRARY_PATH>/libRCAudio.so (default /usr/QuickTransit/lib/ia64);
 * function symbols are then looked up in every library declared so far.
 *
 * Argument word: MIPS register number << 14 | type (a0 = $4 -> 0x10030,
 * a1 -> 0x14030, ...; QuickTransit's own stubs mostly start at a1).
 * Types: 0x30 int (sign-extended), 0x50 unsigned, 0x90 pointer.
 * Return word 01008050: v0 (the native int; it arrived sign-extended in the
 * probe).  The C prototypes declare `long long' and callers truncate, so
 * nothing depends on how QuickTransit fills the upper half.
 */
	.abicalls
	.set	noreorder
	.text

#define A_INT(r)	.word (((r) << 14) | 0x30)
#define A_UINT(r)	.word (((r) << 14) | 0x50)
#define A_PTR(r)	.word (((r) << 14) | 0x90)
#define RET		.word 0x01008050

#define FUNC(name, sym)		\
	.globl	name;		\
	.hidden	name;		\
	.type	name, @function;\
	.align	2;		\
name:	.word	0x7c000002;	\
	.word	0x02000000;	\
	.asciz	sym;		\
	.align	2

#define END(name)		\
	RET;			\
	.word	0x7c000002;	\
	jr	$31;		\
	nop;			\
	.size	name, . - name

	.globl	qta_libdecl
	.hidden	qta_libdecl
	.type	qta_libdecl, @function
	.align	2
qta_libdecl:
	.word	0x7c000000
	.word	0x02000000
	.asciz	"libRCAudio.so"
	.align	2
	.word	0x7c000000
	jr	$31
	nop
	.size	qta_libdecl, . - qta_libdecl

/* long long qta_open(int rate, int channels, int fmt, int queue_frames,
                       unsigned floatmax_bits) */
FUNC(qta_open, "RC_qta_open")
	A_INT(4); A_INT(5); A_INT(6); A_INT(7); A_UINT(8)
END(qta_open)

/* long long qta_write(int h, const void *buf, int frames) */
FUNC(qta_write, "RC_qta_write")
	A_INT(4); A_PTR(5); A_INT(6)
END(qta_write)

/* long long qta_filled(int h) */
FUNC(qta_filled, "RC_qta_filled")
	A_INT(4)
END(qta_filled)

/* long long qta_delay(int h) */
FUNC(qta_delay, "RC_qta_delay")
	A_INT(4)
END(qta_delay)

/* long long qta_close(int h, int drain) */
FUNC(qta_close, "RC_qta_close")
	A_INT(4); A_INT(5)
END(qta_close)
