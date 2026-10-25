; idt_stubs32.s -- 32-bit protected-mode interrupt/exception entry stubs
;
; 32-bit code uses cdecl (arguments on the stack). Stack on entry to
; isr_common / irq_common (low -> high): vector, error code, EIP, CS, EFLAGS.
; pusha adds 32 bytes: [esp+32]=vector, [esp+36]=error code, [esp+40]=EIP.

bits 32

extern exception_handler
extern irq_handler

%macro ISR_NOERRCODE 1
    global isr_stub_%1
    isr_stub_%1:
        push 0              ; dummy error code
        push %1             ; vector number
        jmp isr_common
%endmacro

%macro ISR_ERRCODE 1
    global isr_stub_%1
    isr_stub_%1:
        push %1             ; vector number (CPU pushed the error code)
        jmp isr_common
%endmacro

%macro IRQ_STUB 2
    global irq_stub_%1
    irq_stub_%1:
        push 0
        push %2             ; vector number (32 + IRQ)
        jmp irq_common
%endmacro

section .text

; void exception_handler(vector, error_code, eip)
isr_common:
    pusha                       ; eax,ecx,edx,ebx,esp(orig),ebp,esi,edi
    cld
    mov eax, [esp + 40]         ; EIP
    push eax
    mov eax, [esp + 40]         ; error code (shifted by the push above)
    push eax
    mov eax, [esp + 40]         ; vector
    push eax
    call exception_handler
    add esp, 12
    popa
    add esp, 8                  ; drop vector + error code
    iretd

; void irq_handler(vector)
irq_common:
    pusha
    cld
    mov eax, [esp + 32]         ; vector
    push eax
    call irq_handler
    add esp, 4
    popa
    add esp, 8
    iretd


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

section .rodata
align 4
global isr_stub_table
isr_stub_table:
    dd isr_stub_0
    dd isr_stub_1
    dd isr_stub_2
    dd isr_stub_3
    dd isr_stub_4
    dd isr_stub_5
    dd isr_stub_6
    dd isr_stub_7
    dd isr_stub_8
    dd isr_stub_9
    dd isr_stub_10
    dd isr_stub_11
    dd isr_stub_12
    dd isr_stub_13
    dd isr_stub_14
    dd isr_stub_15
    dd isr_stub_16
    dd isr_stub_17
    dd isr_stub_18
    dd isr_stub_19
    dd isr_stub_20
    dd isr_stub_21
    dd isr_stub_22
    dd isr_stub_23
    dd isr_stub_24
    dd isr_stub_25
    dd isr_stub_26
    dd isr_stub_27
    dd isr_stub_28
    dd isr_stub_29
    dd isr_stub_30
    dd isr_stub_31
global irq_stub_table
irq_stub_table:
    dd irq_stub_0
    dd irq_stub_1
    dd irq_stub_2
    dd irq_stub_3
    dd irq_stub_4
    dd irq_stub_5
    dd irq_stub_6
    dd irq_stub_7
    dd irq_stub_8
    dd irq_stub_9
    dd irq_stub_10
    dd irq_stub_11
    dd irq_stub_12
    dd irq_stub_13
    dd irq_stub_14
    dd irq_stub_15
