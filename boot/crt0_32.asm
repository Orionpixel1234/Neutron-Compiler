[bits 32]

global _start
extern main

section .text
_start:
    xor   ebp, ebp      ; mark bottom of call stack

    ; At _start: [esp]=argc, [esp+4]=argv
    ; Pop argc so esp now points at argv[]
    pop   eax           ; eax = argc
    mov   ecx, esp      ; ecx = argv

    push  ecx           ; push argv
    push  eax           ; push argc
    call  main
    add   esp, 8

    ; exit(eax) via int 0x80
    mov   ebx, eax      ; exit code
    mov   eax, 1        ; SYS_exit = 1
    int   0x80
