    .intel_syntax noprefix
    .global main
    .text
main:
    mov eax, 1          # sys_write (x86_64 Linux)
    mov edi, 1          # fd = stdout
    lea rsi, [rip + msg]
    mov edx, 13
    syscall
    xor eax, eax        # return 0
    ret
    .section .rodata
msg:
    .ascii "Hello world!\n"
