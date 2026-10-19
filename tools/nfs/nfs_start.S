| PrismNet NFS client handler: entry point. Linked first, no startup code:
| a DOS handler is started as a process by Mount and gets its startup
| packet on pr_MsgPort. GPL-3.0-only, see LICENSE.

    .text
    .globl  _start
_start:
    jmp     _handler_main

    .globl  _pnfs_version
_pnfs_version:
    .ascii  "$VER: prismnfs-handler 0.3 (17.10.2026) PrismNet NFSv3 client"
    .byte   0
    .even
