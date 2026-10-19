| PrismNet SMB client handler: entry point. Linked first, no startup code:
| a DOS handler is started as a process by Mount and gets its startup
| packet on pr_MsgPort. GPL-3.0-only, see LICENSE.

    .text
    .globl  _start
_start:
    jmp     _handler_main

    .globl  _psmb_version
_psmb_version:
    .ascii  "$VER: prismsmb-handler 0.3 (18.10.2026) PrismNet SMB client"
    .byte   0
    .even

| Hook entry for C hook functions (like amiga.lib's HookEntry): Intuition
| calls h_Entry with a0 = hook, a2 = object, a1 = message; this passes
| them on the stack to h_SubEntry(hook, object, message).
    .globl  _pnfs_hookentry
_pnfs_hookentry:
    move.l  a1,-(sp)
    move.l  a2,-(sp)
    move.l  a0,-(sp)
    move.l  12(a0),a0                   | h_SubEntry
    jsr     (a0)
    lea     12(sp),sp
    rts
