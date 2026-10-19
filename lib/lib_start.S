| PrismNet bsdsocket.library - ROMTag and strings.
| GPL-3.0-only, see LICENSE.
|
| RTF_AUTOINIT is not used: rt_Init points straight at Init, which calls the
| C initialiser (pn_lib_init: MakeLibrary + AddLibrary). For a non-autoinit
| resident exec passes the segment list in a0 (some loaders also in d0).

    .text

| Executing the library as a program just returns -1.
Start:
    moveq   #-1,d0
    rts
    .even

RomTag:
    .word   0x4AFC              | RTC_MATCHWORD
    .long   RomTag              | rt_MatchTag
    .long   EndSkip             | rt_EndSkip
    .byte   0                   | rt_Flags (no RTF_AUTOINIT)
    .byte   4                   | rt_Version
    .byte   9                   | rt_Type = NT_LIBRARY
    .byte   0                   | rt_Pri
    .long   _pn_libname         | rt_Name
    .long   _pn_idstring        | rt_IdString
    .long   Init                | rt_Init
EndSkip:
    .even

    .globl  _pn_libname
_pn_libname:
    .ascii  "bsdsocket.library"
    .byte   0
    .even

    .globl  _pn_idstring
_pn_idstring:
    .ascii  "PrismNet bsdsocket.library 4.3 (18.10.2026)"
    .byte   13,10,0
    .ascii  "$VER: bsdsocket.library 4.3 (18.10.2026) PrismNet"
    .byte   0
    .even

| d0/a0 = segment list, a6 = ExecBase
Init:
    tst.l   d0
    bne.s   1f
    move.l  a0,d0
1:
    jmp     _pn_lib_init
