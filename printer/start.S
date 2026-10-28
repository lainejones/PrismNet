| prismprint.device - ROMTag and strings.
| Part of PrismNet. GPL-3.0-only, see LICENSE.
|
| No RTF_AUTOINIT: rt_Init points at Init, which calls the C initialiser
| (pp_init: MakeLibrary + AddDevice). Exec passes the segment list in a0 for
| a non-autoinit resident (some loaders also in d0).
| PP_PARALLEL builds the same device under the name parallel.device (for
| OS 3.1, whose printer.device only opens parallel.device or serial.device).

    .text

| Executing the device as a program just returns -1.
Start:
    moveq   #-1,d0
    rts
    .even

RomTag:
    .word   0x4AFC              | RTC_MATCHWORD
    .long   RomTag              | rt_MatchTag
    .long   EndSkip             | rt_EndSkip
    .byte   0                   | rt_Flags (no RTF_AUTOINIT)
    .byte   1                   | rt_Version
    .byte   3                   | rt_Type = NT_DEVICE
    .byte   0                   | rt_Pri
    .long   _pp_devname         | rt_Name
    .long   _pp_idstring        | rt_IdString
    .long   Init                | rt_Init
EndSkip:
    .even

    .globl  _pp_devname
_pp_devname:
#ifdef PP_PARALLEL
    .ascii  "parallel.device"
#else
    .ascii  "prismprint.device"
#endif
    .byte   0
    .even

    .globl  _pp_idstring
_pp_idstring:
    .ascii  "prismprint.device 1.0 (28.10.2026)"
    .byte   13,10,0
    .ascii  "$VER: prismprint.device 1.0 (28.10.2026) PrismNet network printer port"
    .byte   0
    .even

| d0/a0 = segment list, a6 = ExecBase
Init:
    tst.l   d0
    bne.s   1f
    move.l  a0,d0
1:
    jmp     _pp_init
