# BrackOS

A hobby x86-64 kernel, built from scratch in C and assembly. It boots via GRUB (Multiboot2) into 64-bit long mode and drops you into a text-mode shell backed by a real FAT32 filesystem.

## Features

- 64-bit long mode boot (Multiboot2 + GRUB), with paging set up across the first 4 GB
- Interrupt handling (IDT + PIC) with a working PIT timer and RTC clock
- PS/2 keyboard and mouse drivers
- A FAT32 filesystem driver (ATA/PIO) with read, write, and delete
- A built-in text editor (`paper`) with arrow-key navigation
- A VGA text-mode shell with 20+ commands
- An experimental framebuffer GUI (early / unstable — see Known Issues)

## Shell Commands

| Command | Description |
|---|---|
| `help` | List available commands |
| `echo <text>` | Print text |
| `clear` | Clear the screen |
| `info` | System information |
| `mem` | Physical memory map (read from GRUB) |
| `cpu` | CPU vendor, model, and features (via CPUID) |
| `time` / `date` | Current time / date from the RTC |
| `uptime` | Time since boot (via the PIT timer) |
| `ver` / `credits` | Version and credits |
| `mouse` | Mouse position and button state |
| `fs` | Filesystem (FAT32) info |
| `gui` | Switch to the graphical desktop *(experimental)* |
| `shutdown` / `reboot` | Power off / reset |
| `repeat N <cmd>` | Run a command N times |
| `ls [path]` | List a directory |
| `cd <path>` | Change directory |
| `mkdir <name>` | Create a directory |
| `rmdir <name>` | Remove a directory |
| `nf <name>` | Create a new empty file |
| `rm <name>` | Delete a file |
| `paper <file>` | Open the text editor |

## Architecture

```
boot.s        -> Multiboot2 header, long-mode + paging setup, jumps into kmain()
kernel.c      -> Shell, command dispatch, kernel entry point
idt_stubs.s   -> Interrupt/exception entry stubs
idt.c         -> IDT setup, PIC remapping, interrupt/exception handlers
fat32.c       -> FAT32 driver (ATA PIO disk access, read/write/delete)
fs_shell.c    -> Filesystem shell commands (ls, cd, mkdir, rm, ...)
paper.c       -> Text editor
vga.c         -> VGA text-mode output
keyboard.c / mouse.c -> PS/2 drivers
timer.c / rtc.c       -> PIT timer and real-time clock
fb.c / font.c / window.c / cursor.c / gui_commands.c -> Framebuffer GUI
cpu.c / meminfo.c / multiboot.c -> CPUID info and the GRUB-provided memory map
```

## Status

This is the first version confirmed to build and boot successfully (in QEMU). It's an early-stage hobby OS — expect rough edges.

### Known Issues

- No memory allocator yet (no `malloc`/`kmalloc`); nothing can allocate memory dynamically
- The `gui` command is unstable — it needs a proper VBE/VESA mode switch and currently has no reliable framebuffer to draw to
- `rmdir` deletes the directory entry but does not free its clusters
- The `paper` editor clips lines at 79 characters
- Tested only in QEMU so far; real hardware (especially UEFI-only machines, AHCI/SATA disks, and USB-only keyboards) is untested and likely needs more driver work

### Roadmap

- Physical page allocator + `kmalloc`
- Shell quality of life: command history, better line editing
- `sleep` command (timer support already exists)
- Fix the GUI framebuffer/mode-switch path
- A custom bootloader, as a separate project, to replace GRUB

## License

TBD.
