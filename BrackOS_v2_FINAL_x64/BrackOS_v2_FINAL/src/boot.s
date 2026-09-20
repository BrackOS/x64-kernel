; boot.s -- Multiboot2 header + x86-64 entry point
; This is what GRUB looks for to know how to load and start our kernel.

bits 32

; Multiboot2 header
MULTIBOOT2_MAGIC equ 0xe85250d6
MULTIBOOT2_ARCH equ 0  ; i386
MULTIBOOT2_HEADER_LEN equ (multiboot_header_end - multiboot_header_start)
MULTIBOOT2_CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LEN)

align 8
multiboot_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd MULTIBOOT2_HEADER_LEN
    dd MULTIBOOT2_CHECKSUM
    
    ; End tag
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .bss
align 16
stack_bottom:
    resb 16384  ; 16 KiB stack
stack_top:

; Page tables for paging setup
align 4096
pml4:
    resq 512
pdpt:
    resq 512
pdt:
    resq 512

section .text
bits 32
global _start
extern kernel_main

_start:
    ; Set up stack (still in 32-bit mode)
    mov esp, stack_top
    
    ; Save multiboot info
    mov edi, ebx        ; Multiboot info in edi
    
    ; Set up minimal paging for 64-bit mode
    ; PML4[0] -> PDPT
    mov eax, pdpt
    or eax, 3           ; Present + Writable
    mov [pml4], eax
    
    ; PDPT[0] -> PDT
    mov eax, pdt
    or eax, 3           ; Present + Writable
    mov [pdpt], eax
    
    ; PDT[0] -> 2MB page, identity mapped
    mov eax, 0x000000
    or eax, 0x83        ; Present + Writable + 2MB page
    mov [pdt], eax
    
    ; Set CR3 to PML4
    mov eax, pml4
    mov cr3, eax
    
    ; Enable PAE (Physical Address Extension)
    mov eax, cr4
    or eax, 0x20        ; PAE bit
    mov cr4, eax
    
    ; Enable long mode (LM bit in EFER)
    mov ecx, 0xC0000080 ; EFER MSR
    rdmsr
    or eax, 0x100       ; LM bit
    wrmsr
    
    ; Enable paging
    mov eax, cr0
    or eax, 0x80000000  ; PG bit
    mov cr0, eax
    
    ; Far jump to 64-bit code
    lgdt [gdt_pointer]
    jmp 0x08:long_mode_start

bits 64
long_mode_start:
    ; Set up 64-bit segments
    mov ax, 0x10
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Set up 64-bit stack
    mov rsp, stack_top
    
    ; Clear registers
    xor rbp, rbp
    
    ; Call kernel_main
    call kernel_main
    
.hang:
    hlt
    jmp .hang

; GDT for 64-bit
align 8
gdt:
    dq 0                ; Null descriptor
    dq 0x00af9a000000ffff  ; Code descriptor (64-bit)
    dq 0x00af92000000ffff  ; Data descriptor

gdt_pointer:
    dw $ - gdt - 1
    dq gdt
