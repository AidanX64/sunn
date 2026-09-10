    .intel_syntax noprefix
    .global hello_asm_value
    .text
    /* int hello_asm_value(void) — x86_64 System V / Windows identical here:
       single integer return in eax/rax, no stack use. */
hello_asm_value:
    mov eax, 44
    ret
