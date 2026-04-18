; crt0.asm — program entry point (_start)
; Calls main(argc, argv) then exits via syscall.
[bits 64]
global _start
extern main

section .text
_start:
    xor  rbp, rbp          ; mark outermost frame
    pop  rdi               ; argc
    mov  rsi, rsp          ; argv  (points to argv[0])
    and  rsp, -16          ; 16-byte align before call
    call main
    mov  rdi, rax          ; exit code = return value of main
    mov  rax, 60           ; SYS_exit
    db 0x0F, 0x05          ; syscall
