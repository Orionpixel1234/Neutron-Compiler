; crtn.asm — epilogues for .init / .fini sections
[bits 64]

section .init
    pop  rbp
    ret

section .fini
    pop  rbp
    ret
