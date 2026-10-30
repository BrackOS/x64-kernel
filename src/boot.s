; boot.s -- Multiboot2 header + x86-64 entry point
; GRUB reads the header below to know how to load the kernel and where to jump.

bits 32

; Symbols provided by linker.ld
extern __kernel_start
extern __kernel_load_end
extern __kernel_bss_end

MULTIBOOT2_MAGIC equ 0xe85250d6
MULTIBOOT2_ARCH equ 0  ; i386 is the value Multiboot2 requires (GRUB enters in 32-bit mode)
MULTIBOOT2_HEADER_LEN equ (multiboot_header_end - multiboot_header_start)
MULTIBOOT2_CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LEN)

; Multiboot2 header. It must sit in the first 32 KB of the image.
; The address tag lets GRUB load our flat kernel.bin (no ELF needed).
section .multiboot
align 8
multiboot_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd MULTIBOOT2_HEADER_LEN
    dd MULTIBOOT2_CHECKSUM

    ; Address tag (type 2): where the image lives in memory and how much BSS to zero
    align 8
    dw 2                        ; type
    dw 0                        ; flags
    dd 24                       ; size
    dd __kernel_start           ; header_addr (header is at the image start)
    dd __kernel_start           ; load_addr
    dd __kernel_load_end        ; load_end_addr
    dd __kernel_bss_end         ; bss_end_addr

    ; Entry address tag (type 3): where GRUB jumps
    align 8
    dw 3
    dw 0
    dd 12
    dd _start

    ; (No framebuffer request tag: GRUB leaves the screen in standard
    ; VGA text mode at boot, which is what the shell writes to. The GUI
    ; command switches to graphics mode later, on demand.)

    ; End tag
    align 8
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .bss align=4096
global stack_guard
global stack_top
global pml4
global pdpt
global pdt

; Kernel stack, with a guard page below it. paging_init() unmaps the guard
; page, so running off the bottom of the stack faults instead of silently
; overwriting whatever sits below.
align 4096
stack_guard:
    resb 4096
stack_bottom:
    resb 32768  ; 32 KiB stack
stack_top:

; Page tables for paging setup
align 4096
pml4:
    resq 512
pdpt:
    resq 512
pdt:
    resq 2048   ; 4 page directories x 512 entries = 4 GB of 2 MB pages

section .text
bits 32
global _start
extern kmain

_start:
    ; Set up stack (still in 32-bit mode)
    mov esp, stack_top

    ; Save multiboot info pointer (GRUB passes it in ebx). cpuid below
    ; clobbers ebx, so this must happen first. edi is not touched again.
    mov edi, ebx

    ; GRUB leaves 0x36d76289 in eax when it loaded us via Multiboot2
    cmp eax, 0x36d76289
    jne .no_multiboot

    ; Is the cpuid instruction available? (it is if we can flip EFLAGS.ID)
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd                       ; restore the original flags
    cmp eax, ecx
    je .no_long_mode

    ; Does cpuid offer the extended leaves (0x80000001)?
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    ; Is the long mode (LM) bit set? (EDX bit 29)
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; PML4[0] -> PDPT
    mov eax, pdpt
    or eax, 3                   ; Present + Writable
    mov [pml4], eax

    ; PDPT[0..3] -> four page directories (each covers 1 GB)
    xor ecx, ecx
.fill_pdpt:
    mov eax, ecx
    shl eax, 12                 ; i * 4096 (each page directory is 4 KB)
    add eax, pdt
    or eax, 3
    mov [pdpt + ecx*8], eax
    mov dword [pdpt + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 4
    jne .fill_pdpt

    ; PDT entries: 2048 x 2 MB pages, identity mapped across 0..4 GB
    xor ecx, ecx
.fill_pdt:
    mov eax, ecx
    shl eax, 21                 ; i * 2 MB
    or eax, 0x83                ; Present + Writable + 2 MB page
    mov [pdt + ecx*8], eax
    mov dword [pdt + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 2048
    jne .fill_pdt

    ; Set CR3 to PML4
    mov eax, pml4
    mov cr3, eax

    ; Enable PAE (Physical Address Extension)
    mov eax, cr4
    or eax, 0x20                ; PAE bit
    mov cr4, eax

    ; Enable long mode (LM bit in EFER)
    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or eax, 0x100               ; LM bit
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 0x80000000          ; PG bit
    mov cr0, eax

    ; Load GDT and far jump to 64-bit code
    lgdt [gdt_pointer]
    jmp 0x08:long_mode_start

; ---- Boot errors: print a message on the VGA text screen and halt ----
.no_multiboot:
    mov esi, msg_no_multiboot
    jmp .fail
.no_long_mode:
    mov esi, msg_no_long_mode
.fail:
    mov edi, 0xB8000
    mov ecx, 80 * 25
    mov ax, 0x4F20              ; white on red, space: clear the screen red
    rep stosw
    mov edi, 0xB8000
    mov ah, 0x4F
.print:
    lodsb
    test al, al
    jz .halt32
    stosw
    jmp .print
.halt32:
    cli
    hlt
    jmp .halt32

msg_no_multiboot: db 'BrackOS: not started by a Multiboot2 loader. Use GRUB.', 0
msg_no_long_mode: db 'BrackOS: this CPU has no 64-bit mode (x86-64 required).', 0

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

    ; Call kmain(mbi_addr). edi still holds the Multiboot2 info pointer.
    call kmain

.hang:
    hlt
    jmp .hang

; GDT for 64-bit
align 8
gdt:
    dq 0                        ; Null descriptor
    dq 0x00af9a000000ffff       ; Code descriptor (64-bit)
    dq 0x00af92000000ffff       ; Data descriptor

gdt_pointer:
    dw $ - gdt - 1
    dq gdt
