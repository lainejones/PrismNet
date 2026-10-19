| PrismNet usergroup.library - ROMTag and strings.
| GPL-3.0-only, see LICENSE.  (Same scheme as lib_start.S.)

    .text

Start:
    moveq   #-1,d0
    rts
    .even

RomTag:
    .word   0x4AFC
    .long   RomTag
    .long   EndSkip
    .byte   0                   | no RTF_AUTOINIT
    .byte   4                   | version
    .byte   9                   | NT_LIBRARY
    .byte   0
    .long   _ug_libname
    .long   _ug_idstring
    .long   Init
EndSkip:
    .even

    .globl  _ug_libname
_ug_libname:
    .ascii  "usergroup.library"
    .byte   0
    .even

    .globl  _ug_idstring
_ug_idstring:
    .ascii  "PrismNet usergroup.library 4.3 (18.10.2026)"
    .byte   13,10,0
    .ascii  "$VER: usergroup.library 4.3 (18.10.2026) PrismNet"
    .byte   0
    .even

Init:
    tst.l   d0
    bne.s   1f
    move.l  a0,d0
1:
    jmp     _ug_lib_init