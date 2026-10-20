# BrackOS x86-64

A minimal but functional 64-bit operating system kernel for x86-64 architecture, bootable via GRUB and runnable on real hardware or QEMU.

**Architecture:** x86-64 (AMD64)  
**Boot:** GRUB 2 Multiboot2  
**CPU Mode:** 64-bit Long Mode  
**Display:** VGA text mode (80×25)  
**Input:** PS/2 keyboard  
**Shell:** Simple command-line interface

---

## Quick Facts

- **True 64-bit kernel** running in long mode with paging enabled
- **Multiboot2 bootloader** supporting both BIOS and UEFI firmware
- **Identity-mapped 2MB pages** with full page table hierarchy (PML4/PDPT/PDT)
- **VGA text display** with 16 foreground and 8 background colors
- **PS/2 keyboard input** with shift key support via polling
- **Simple shell** with three commands: help, echo, clear
- **~380 lines of C code** + ~120 lines of x86-64 assembly

---

## Build Requirements

**Linux/macOS (x86-64):**
```bash
sudo apt-get install build-essential nasm grub-common xorriso qemu-system-x86
```

**Windows 11 (MSYS2):**
1. Download and install [MSYS2](https://www.msys2.org/)
2. In MSYS2 terminal:
   ```bash
   pacman -Syu
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils nasm xorriso grub
   ```

---

## Compile

```bash
make clean && make iso
```

Creates `brackos.iso` (bootable ISO image).

---

## Run in QEMU

```bash
make run
```

Or manually:
```bash
qemu-system-x86_64 -cdrom brackos.iso
```

Output:
```
BrackOS x86-64 booted successfully.
Type 'help' for a list of commands.

BrackOS> █
```

Exit QEMU: `Ctrl+A` then `X`

---

## Boot on Real Hardware

1. Write ISO to USB:
   - Download [Balena Etcher](https://www.balena.io/etcher/)
   - Select `brackos.iso`
   - Select USB drive
   - Click "Flash"

2. Boot x86-64 PC from USB:
   - Insert USB
   - Restart computer
   - Press `F9` (or `F12`/`Delete` - varies by manufacturer) during startup
   - Select USB device from boot menu
   - System boots

3. Use the shell:
   ```
   BrackOS> help
   BrackOS> echo Hello from BrackOS!
   BrackOS> clear
   ```

---

## Shell Commands

### `help`
Displays available commands.

### `echo <text>`
Prints text to screen. Example:
```
BrackOS> echo Hello World
Hello World
```

### `clear`
Clears the screen and moves cursor to top-left.

### Unknown Command
Displays error in red: `error: unknown command: [input]`

---

## Technical Details

### Boot Process
1. GRUB 2 loads kernel as Multiboot2 image
2. Boot code (_start) starts in 32-bit protected mode
3. Kernel sets up paging (PML4 → PDPT → PDT, 2MB pages)
4. Enables PAE (Physical Address Extension)
5. Enables Long Mode via EFER MSR
6. Loads 64-bit GDT and performs far jump
7. Runs in 64-bit long mode, calls kernel_main()

### Memory Layout
- **Kernel load:** 0x100000 (1 MB)
- **Stack:** 16 KB in .bss section
- **VGA buffer:** 0xB8000 (identity-mapped)
- **Page tables:** PML4, PDPT, PDT (each 4 KB)
- **Addressing:** Identity mapping (virtual = physical) for first 2 MB

### Display
- **Mode:** VGA text mode
- **Resolution:** 80 columns × 25 rows
- **Memory:** Direct access to 0xB8000
- **Colors:** 16 foreground (0-15) + 8 background (0-7)
- **Features:** Automatic scrolling, backspace support

### Input
- **Hardware:** PS/2 keyboard only
- **Method:** Port polling (0x60 data, 0x64 status)
- **Scancodes:** Set 1 with shift key support
- **Blocking:** Input waits for keypress

### Shell
- **Command buffer:** 256 characters max
- **Parsing:** Case-sensitive, exact matching
- **Input echo:** Characters displayed as typed
- **Backspace:** Supported
- **Error display:** Red text

---

## Architecture

### CPU Features Used
- 64-bit long mode
- Paging (4-level page tables)
- PAE (Physical Address Extension)
- GDT (Global Descriptor Table)

### CPU Features NOT Used
- Interrupts (disabled)
- Exceptions (no handlers)
- IDT (not implemented)
- TSS (not implemented)
- MSRs (infrastructure present but unused)

---

## Known Limitations

- **No exception handling** - Page faults, GPF, etc. not handled
- **No interrupts** - All hardware interaction via polling
- **No multitasking** - Single process only
- **No filesystem** - No disk I/O
- **No heap** - No dynamic memory allocation
- **No protected memory** - Single privilege level
- **No serial I/O** - No serial port support
- **No networking** - No network drivers
- **No timers** - No clock or sleep functions
- **No PCI/ACPI** - No bus enumeration
- **Text mode only** - No graphics mode

---

## Source Structure

| File | Lines | Purpose |
|------|-------|---------|
| `src/boot.s` | 124 | Multiboot2 header, 32→64-bit transition, paging setup, GDT |
| `src/kernel.c` | 106 | Main shell loop, command parser, line reader |
| `src/vga.c` | 94 | VGA text display driver (0xB8000 access) |
| `src/keyboard.c` | 103 | PS/2 keyboard driver with scancode translation |
| `src/io.h` | 16 | I/O port primitives (inb/outb, read_msr/write_msr) |
| `src/vga.h` | 14 | VGA function declarations |
| `src/keyboard.h` | 9 | Keyboard function declarations |
| `src/linker.ld` | 28 | Memory layout, section alignment, kernel base address |
| `iso/boot/grub/grub.cfg` | 13 | GRUB 2 boot configuration (Multiboot2) |

**Total:** ~507 lines of code (C + assembly + config)

---

## Bootloader Details

### Multiboot2 Compliance
- **Magic number:** 0xe85250d6
- **Architecture:** i386 (compatible with x86-64)
- **Header validation:** Checksum present
- **End tag:** Present

### GRUB Configuration
- **Boot protocol:** Multiboot2
- **Kernel image:** `/boot/brackos.bin`
- **Graphics:** Auto-detect UEFI/BIOS graphics mode
- **Timeout:** 0 seconds (immediate boot)

### Supported Firmware
- **BIOS:** Yes (legacy boot mode)
- **UEFI:** Yes (graphics mode auto-detection via grub.cfg)

---

## Compilation Details

### Compiler Flags
```
-m64              : 64-bit target
-ffreestanding    : Freestanding environment
-fno-stack-protector : Disable stack canaries
-mcmodel=kernel   : Large kernel code model
-nostdlib         : No standard library
-fno-builtin      : Disable compiler builtins
```

### Assembler
- NASM with `-f elf64` output format

### Linker
- GNU ld, `-m elf_x86_64` emulation
- Entry point: `_start`
- Load address: 0x100000

---

## Testing

Tested on:
- QEMU x86-64 emulation
- Real x86-64 PC hardware (BIOS and UEFI)

---

## File Structure

```
BrackOS/
├── src/
│   ├── boot.s           x86-64 bootloader + paging setup
│   ├── kernel.c         Main kernel + shell
│   ├── vga.c/vga.h      VGA text display driver
│   ├── keyboard.c/h     PS/2 keyboard driver
│   ├── io.h             I/O port primitives
│   └── linker.ld        Memory layout (0x100000 start)
├── iso/
│   └── boot/grub/
│       └── grub.cfg     GRUB 2 boot configuration
├── Makefile             Build system
└── README.md            This file
```

---

## Next Steps

1. **Compile:** `make clean && make iso`
2. **Test:** `make run` (QEMU)
3. **Deploy:** Write to USB with Balena Etcher
4. **Boot:** Insert USB, press F9 at startup, select USB

---

## Technical Specification

For detailed technical documentation including CPU feature details, memory layout, I/O addressing, and hardware limitations, see `TECHNICAL_SPECIFICATION.md`.

---

**Ready to build and boot!** 🚀
