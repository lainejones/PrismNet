| PrismNet - 68000 arithmetic helpers for the libraries (no libnix startup).
| GPL-3.0-only, see LICENSE.

    .text

| ---- 68000 arithmetic helpers ---------------------------------------------
| libnix's versions call utility.library through UtilityBase, which libnix's
| startup opens - and a library has no startup. Stack ABI: args at 4(sp),
| 8(sp); result in d0; d2-d7/a2-a6 preserved.

    .globl  ___mulsi3
___mulsi3:                      | d0 = a * b (low 32 bits)
    move.w  4(sp),d0            | a.hi
    mulu.w  10(sp),d0           | a.hi * b.lo
    move.w  6(sp),d1            | a.lo
    mulu.w  8(sp),d1            | a.lo * b.hi
    add.w   d1,d0
    swap    d0
    clr.w   d0
    move.w  6(sp),d1            | a.lo
    mulu.w  10(sp),d1           | a.lo * b.lo
    add.l   d1,d0
    rts

    .globl  ___udivsi3
___udivsi3:                     | d0 = a / b (unsigned)
    movem.l d2-d4,-(sp)
    move.l  16(sp),d0           | dividend
    move.l  20(sp),d3           | divisor
    bsr.s   udivmod
    move.l  d4,d0
    movem.l (sp)+,d2-d4
    rts

    .globl  ___umodsi3
___umodsi3:                     | d0 = a % b (unsigned)
    movem.l d2-d4,-(sp)
    move.l  16(sp),d0
    move.l  20(sp),d3
    bsr.s   udivmod
    move.l  d2,d0
    movem.l (sp)+,d2-d4
    rts

    .globl  ___divsi3
___divsi3:                      | d0 = a / b (signed, truncating)
    movem.l d2-d5,-(sp)
    move.l  20(sp),d0
    move.l  24(sp),d3
    moveq   #0,d5               | result sign
    tst.l   d0
    bpl.s   1f
    neg.l   d0
    not.l   d5
1:  tst.l   d3
    bpl.s   2f
    neg.l   d3
    not.l   d5
2:  bsr     udivmod
    move.l  d4,d0
    tst.l   d5
    beq.s   3f
    neg.l   d0
3:  movem.l (sp)+,d2-d5
    rts

    .globl  ___modsi3
___modsi3:                      | d0 = a % b (signed, sign of a)
    movem.l d2-d5,-(sp)
    move.l  20(sp),d0
    move.l  24(sp),d3
    moveq   #0,d5
    tst.l   d0
    bpl.s   1f
    neg.l   d0
    not.l   d5
1:  tst.l   d3
    bpl.s   2f
    neg.l   d3
2:  bsr     udivmod
    move.l  d2,d0
    tst.l   d5
    beq.s   3f
    neg.l   d0
3:  movem.l (sp)+,d2-d5
    rts

| in: d0 dividend, d3 divisor; out: d4 quotient, d2 remainder (shift-subtract)
udivmod:
    moveq   #0,d2
    moveq   #0,d4
    moveq   #31,d1
2:
    add.l   d2,d2
    btst    d1,d0
    beq.s   3f
    addq.l  #1,d2
3:
    cmp.l   d3,d2
    blo.s   4f
    sub.l   d3,d2
    bset    d1,d4
4:
    dbra    d1,2b
    rts
| ---- direct calls ----------------------------------------------------------
| LONG pn_on_stack(APTR fn, APTR arg, APTR top): fn(arg) on the stack that
| ends at `top` (the core's request handler, run in the caller's task on a
| private stack). The caller's sp is kept on the new stack.

    .globl  _pn_on_stack
_pn_on_stack:
    move.l  4(sp),a0            | fn
    move.l  8(sp),d0            | arg
    move.l  12(sp),a1           | new stack top
    move.l  sp,-(a1)            | the caller's sp, on the new stack
    move.l  a1,sp
    move.l  d0,-(sp)
    jsr     (a0)                | d0 = fn(arg)
    addq.l  #4,sp
    move.l  (sp),sp             | back on the caller's stack
    rts
