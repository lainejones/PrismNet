| PrismNet - Internet checksum for lwIP (LWIP_CHKSUM), 68000 and up.
| GPL-3.0-only, see LICENSE.
|
| u16_t pn_chksum(const void *data, int len)
| Same contract as lwIP's lwip_standard_chksum(): the non-inverted 16-bit
| one's complement sum in host (= network, big-endian) order. `data` may be
| odd: the first byte is then summed as a low byte and the result byte-
| swapped, as lwIP expects. Longwords are read at even addresses only, which
| the 68000 allows. The inner loop does 32 bytes per pass with addx.l.

    .text
    .globl  _pn_chksum
_pn_chksum:
    movem.l d2-d5,-(sp)
    move.l  20(sp),a0           | data
    move.l  24(sp),d1           | len
    moveq   #0,d0               | sum
    moveq   #0,d4               | zero (for addx)
    moveq   #0,d5               | odd start flag
    tst.l   d1
    ble     .done
    move.w  a0,d2
    btst    #0,d2
    beq.s   .even
    moveq   #1,d5
    move.b  (a0)+,d0            | first byte is the low byte of its word
    subq.l  #1,d1
.even:
    move.l  d1,d3
    lsr.l   #5,d3               | 32-byte blocks
    beq.s   .longs
    subq.w  #1,d3
    add.l   d4,d4               | clear X
.blk:
    move.l  (a0)+,d2
    addx.l  d2,d0
    move.l  (a0)+,d2
    addx.l  d2,d0
    move.l  (a0)+,d2
    addx.l  d2,d0
    move.l  (a0)+,d2
    addx.l  d2,d0
    move.l  (a0)+,d2
    addx.l  d2,d0
    move.l  (a0)+,d2
    addx.l  d2,d0
    move.l  (a0)+,d2
    addx.l  d2,d0
    move.l  (a0)+,d2
    addx.l  d2,d0
    dbra    d3,.blk
    addx.l  d4,d0               | end-around carry (twice: the first can carry)
    addx.l  d4,d0
.longs:
    move.w  d1,d3
    andi.w  #28,d3
    lsr.w   #2,d3               | remaining longwords (0..7)
    beq.s   .word
    subq.w  #1,d3
    add.l   d4,d4               | clear X
.lng:
    move.l  (a0)+,d2
    addx.l  d2,d0
    dbra    d3,.lng
    addx.l  d4,d0
    addx.l  d4,d0
.word:
    btst    #1,d1
    beq.s   .byte
    moveq   #0,d2
    move.w  (a0)+,d2
    add.l   d2,d0
    addx.l  d4,d0               | cannot carry again: sum <= 0xfffe here
.byte:
    btst    #0,d1
    beq.s   .fold
    moveq   #0,d2
    move.b  (a0),d2
    lsl.w   #8,d2               | trailing byte is a high byte
    add.l   d2,d0
    addx.l  d4,d0
.fold:
    move.l  d0,d2
    swap    d2
    add.w   d2,d0
    addx.w  d4,d0               | lo + hi + carry never exceeds 0xffff
    andi.l  #0xffff,d0
    tst.w   d5
    beq.s   .done
    rol.w   #8,d0               | odd start: swap bytes
.done:
    movem.l (sp)+,d2-d5
    rts
