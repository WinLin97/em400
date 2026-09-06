; CONFIG configs/flop8_empty.ini

; Stand-in for BOSS's CFA subcommand hitting the "completion interrupt
; races the process going to sleep" bug. The fix is a mechanical delay on
; UZFX's completion interrupts.
;
; The other flop8-int-ready-* tests issue PISZ, get EN and start waiting
; within a couple of instructions, so cpu.c's OU+HLT tandem shortcut hides
; the race from them. The CROOK-5 kernel does more: a PISZ answered EN
; makes it BLOCK the calling process - enqueue it on the unit's wait
; queue, mark the PCB I/O-blocked, and context-switch away (~8
; instructions), with the floppy interrupt enabled the whole time; the
; "ready" interrupt's ISR later pulls the process back onto the ready
; queue. If the interrupt is delivered before that enqueue, the ISR finds
; nothing to wake - and once the block path finishes the process is asleep
; for good (the whole system then idles). BOSS's CFA (a ring-3 utility
; that only issues WRIT syscalls) drives a long run of such one-byte-then-
; block writes and hangs on this.
;
; No scheduler or syscalls in a functional test, so this stands in for it:
; the block path is a long instruction stretch and the wait a bare HLT.
; PISZ -> EN -> a few thousand instructions with the interrupt live and
; the handler NOT redirecting control -> blind HLT -> stream the sector,
; over 8 sectors. A ready delivered during the stretch is effectively lost
; and the HLT then hangs (harness timeout). A controller that delays its
; completion interrupt delivers it only after the HLT, and the test
; completes.

	.cpu	mera400

	.include cpu.inc
	.include io.inc

	mcl
	uj	start

	.const	FLOP_CHAN 7
	.const	FLOP_DEV 2
	.const	FLOP FLOP_CHAN\IO_CHAN | FLOP_DEV\IO_DEV
	.const	INT_READY 1
	.const	BOOKKEEPING 2000		; loop trips; ~2 instructions each

	.org	OS_START

; ------------------------------------------------------------------------
mask:	.word	IMASK_CH4_9		; floppy channel only - NOT the timer,
					; so a stuck HLT here stays stuck

; floppy interrupt handler: check the spec, bump a counter, return to
; wherever we were interrupted. Deliberately does NOT redirect the IC -
; the mainline is expected to be sitting in HLT, not mid-bookkeeping.
int_flop:
	md	[STACKP]
	lw	r7, [-SP_SPEC]
	cw	r7, FLOP_DEV\KZ_INT_DEV + INT_READY\KZ_INT_NUM
	jes	.ok
	hlt	076			; wrong interrupt spec
.ok:
	lw	r7, [int_count]
	awt	r7, 1
	rw	r7, [int_count]
	lip

int_count:
	.res	1

; ------------------------------------------------------------------------
start:
	lw	r1, stack
	rw	r1, STACKP
	lw	r1, int_flop
	rw	r1, INTV_CH0 + FLOP_CHAN
	im	mask

	lw	r3, 8			; format 8 sectors this way

next_sector:
	cwt	r3, 0
	jes	test_done
	awt	r3, -1

	; note how many interrupts we have seen before starting the op
	lw	r4, [int_count]

	; first byte of the sector: answered EN
	ou	r1, FLOP | KZ_CMD_DEV_WRITE
	.word	.no1, .en1, .ok1, .pe1
.no1:	hlt	040
.pe1:	hlt	041
.ok1:	hlt	042
.en1:

	; --- bookkeeping window: interrupts live, ready must NOT arrive yet ---
	lw	r5, BOOKKEEPING
.bk:
	awt	r5, -1
	cwt	r5, 0
	jn	.bk

	; if "ready" was already delivered during the bookkeeping, the
	; controller had zero latency - that is the bug this test guards.
	lw	r7, [int_count]
	cw	r7, r4
	jn	too_early

	; blind wait, exactly like a real driver: trust the interrupt to wake us
	hlt	0
	; the interrupt fired and was served; control resumes here

	; stream the rest of the sector - 128 writes answered OK, last flushes
	lw	r2, 128
next_byte:
	cwt	r2, 0
	jes	sector_done
	awt	r2, -1
	ou	r1, FLOP | KZ_CMD_DEV_WRITE
	.word	.no2, .en2, .ok2, .pe2
.no2:	hlt	050
.pe2:	hlt	051
.ok2:	ujs	next_byte
.en2:	hlt	052

sector_done:
	ujs	next_sector

too_early:
	hlt	065			; FAIL: ready interrupt beat the HLT

test_done:
	hlt	077

stack:

; XPCT ir : 0xec3f
; XPCT alarm : 0
