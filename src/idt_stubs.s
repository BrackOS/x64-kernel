; Interrupt Service Routine (ISR) stubs for x86-64
; These stubs save the CPU state, call handlers, and restore

; External handlers (defined in idt.c)
extern exception_handler
extern irq_handler

; Macro to create an exception handler that doesn't push error code
%macro ISR_NOERRCODE 1
    global isr_stub_%1
    isr_stub_%1:
        push 0              ; Push dummy error code
        push %1             ; Push exception number
        jmp isr_common
%endmacro

; Macro to create an exception handler that pushes error code
%macro ISR_ERRCODE 1
    global isr_stub_%1
    isr_stub_%1:
        push %1             ; Push exception number
        jmp isr_common
%endmacro

; Macro to create an IRQ handler
%macro IRQ_STUB 2
    global irq_stub_%1
    irq_stub_%1:
        push 0              ; Dummy error code
        push %2             ; IRQ number
        jmp irq_common
%endmacro

; Common exception handler
isr_common:
    ; Save all registers
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    
    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Get exception number from stack
    mov rdi, [rsp + 48]  ; Vector number (pushed after the dummy error code)
    sub rsp, 8           ; Keep the stack 16-byte aligned for the C call
    call exception_handler
    add rsp, 8
    
    ; Should not return, but restore anyway
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    
    add rsp, 16         ; Remove error code and exception number
    iretq

; Common IRQ handler
irq_common:
    ; Save all registers
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    
    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Get IRQ number from stack
    mov rdi, [rsp + 48]  ; Vector number (32-47 for remapped IRQs)
    sub rsp, 8           ; Keep the stack 16-byte aligned for the C call
    call irq_handler
    add rsp, 8
    
    ; Restore registers
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    
    add rsp, 16         ; Remove error code and IRQ number
    iretq

; Define exception stubs (0-31)
; Most don't push error code; CPU does for some
ISR_NOERRCODE 0     ; Division by zero
ISR_NOERRCODE 1     ; Debug
ISR_NOERRCODE 2     ; Non-maskable interrupt
ISR_NOERRCODE 3     ; Breakpoint
ISR_NOERRCODE 4     ; Overflow
ISR_NOERRCODE 5     ; Bound range exceeded
ISR_NOERRCODE 6     ; Invalid opcode
ISR_NOERRCODE 7     ; Device not available
; 8: Double fault (has error code, but we'll skip for now)
ISR_NOERRCODE 9     ; Coprocessor segment overrun
ISR_NOERRCODE 10    ; Invalid TSS
ISR_NOERRCODE 11    ; Segment not present
ISR_NOERRCODE 12    ; Stack segment fault
ISR_NOERRCODE 13    ; General protection fault
ISR_NOERRCODE 14    ; Page fault
ISR_NOERRCODE 15    ; Reserved
ISR_NOERRCODE 16    ; Floating point exception
ISR_NOERRCODE 17    ; Alignment check
ISR_NOERRCODE 18    ; Machine check
ISR_NOERRCODE 19    ; SIMD floating point
ISR_NOERRCODE 20    ; Virtualization exception
ISR_NOERRCODE 21    ; Control protection exception
ISR_NOERRCODE 22    ; Reserved
ISR_NOERRCODE 23    ; Reserved
ISR_NOERRCODE 24    ; Reserved
ISR_NOERRCODE 25    ; Reserved
ISR_NOERRCODE 26    ; Reserved
ISR_NOERRCODE 27    ; Reserved
ISR_NOERRCODE 28    ; Reserved
ISR_NOERRCODE 29    ; Reserved
ISR_NOERRCODE 30    ; Reserved
ISR_NOERRCODE 31    ; Reserved

; Define IRQ stubs (mapped to 32-47)
; IRQ 0 = timer
IRQ_STUB 0, 32

; IRQ 1 = keyboard
IRQ_STUB 1, 33

; IRQ 12 = mouse
IRQ_STUB 12, 44

; IRQ 14 = ATA disk 1
IRQ_STUB 14, 46
