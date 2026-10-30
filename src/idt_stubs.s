; Interrupt Service Routine (ISR) stubs for x86-64
;
; Every stub pushes a vector number (and a dummy error code when the CPU
; does not push one), then jumps to a common routine that saves ALL
; general-purpose registers, calls into C, restores them and returns.
;
; Stack on entry to isr_common / irq_common (low -> high address):
;   vector, error code, RIP, CS, RFLAGS, RSP, SS
; After the 15 register pushes the stack is 16-byte aligned for the C call.

bits 64

extern exception_handler
extern irq_handler

; Exception without a CPU-pushed error code
%macro ISR_NOERRCODE 1
    global isr_stub_%1
    isr_stub_%1:
        push 0              ; dummy error code
        push %1             ; vector number
        jmp isr_common
%endmacro

; Exception where the CPU already pushed an error code
%macro ISR_ERRCODE 1
    global isr_stub_%1
    isr_stub_%1:
        push %1             ; vector number
        jmp isr_common
%endmacro

; Hardware IRQ: %1 = IRQ number, %2 = vector (32 + IRQ)
%macro IRQ_STUB 2
    global irq_stub_%1
    irq_stub_%1:
        push 0              ; dummy error code
        push %2             ; vector number
        jmp irq_common
%endmacro

%macro SAVE_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

section .text

; void exception_handler(vector, error_code, rip)
isr_common:
    SAVE_REGS
    cld                         ; C code requires DF = 0
    mov rdi, [rsp + 120]        ; vector
    mov rsi, [rsp + 128]        ; error code
    mov rdx, [rsp + 136]        ; RIP of the faulting instruction
    call exception_handler
    RESTORE_REGS
    add rsp, 16                 ; drop vector + error code
    iretq

; void irq_handler(vector)
irq_common:
    SAVE_REGS
    cld
    mov rdi, [rsp + 120]        ; vector (32-47)
    call irq_handler
    RESTORE_REGS
    add rsp, 16
    iretq


ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE 8
ISR_NOERRCODE 9
ISR_ERRCODE 10
ISR_ERRCODE 11
ISR_ERRCODE 12
ISR_ERRCODE 13
ISR_ERRCODE 14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE 17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_ERRCODE 29
ISR_ERRCODE 30
ISR_NOERRCODE 31

IRQ_STUB 0, 32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

; Tables the C code uses to fill the IDT
section .rodata
align 8
global isr_stub_table
isr_stub_table:
    dq isr_stub_0
    dq isr_stub_1
    dq isr_stub_2
    dq isr_stub_3
    dq isr_stub_4
    dq isr_stub_5
    dq isr_stub_6
    dq isr_stub_7
    dq isr_stub_8
    dq isr_stub_9
    dq isr_stub_10
    dq isr_stub_11
    dq isr_stub_12
    dq isr_stub_13
    dq isr_stub_14
    dq isr_stub_15
    dq isr_stub_16
    dq isr_stub_17
    dq isr_stub_18
    dq isr_stub_19
    dq isr_stub_20
    dq isr_stub_21
    dq isr_stub_22
    dq isr_stub_23
    dq isr_stub_24
    dq isr_stub_25
    dq isr_stub_26
    dq isr_stub_27
    dq isr_stub_28
    dq isr_stub_29
    dq isr_stub_30
    dq isr_stub_31
global irq_stub_table
irq_stub_table:
    dq irq_stub_0
    dq irq_stub_1
    dq irq_stub_2
    dq irq_stub_3
    dq irq_stub_4
    dq irq_stub_5
    dq irq_stub_6
    dq irq_stub_7
    dq irq_stub_8
    dq irq_stub_9
    dq irq_stub_10
    dq irq_stub_11
    dq irq_stub_12
    dq irq_stub_13
    dq irq_stub_14
    dq irq_stub_15
