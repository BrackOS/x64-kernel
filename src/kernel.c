#include "vga.h"
#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "fb.h"
#include "font.h"
#include "window.h"
#include "mouse.h"
#include "cursor.h"
#include "recovery.h"
#include "fat32.h"
#include "fs_shell.h"
#include "paper.h"
#include "rtc.h"
#include "timer.h"
#include "cpu.h"
#include "meminfo.h"
#include "heap.h"
#include "readline.h"
#include "multiboot.h"
#include "serial.h"
#include "gdt.h"
#include "paging.h"
#include "pmm.h"
#include <stdint.h>

static void crash_test(const char *what);
#include "string.h"

/* Command buffer */
#define CMD_BUFFER_SIZE 256
char cmd_buffer[CMD_BUFFER_SIZE];
int cmd_pos = 0;

/* GUI mode flag */
int gui_mode = 0;

/* Format time with leading zeros */
void print_time_component(uint8_t val) {
    if (val < 10) vga_print("0");
    vga_print_int(val);
}

void process_command(char *cmd) {
    if (strlen(cmd) == 0) return;
    
    if (strcmp(cmd, "help") == 0) {
        vga_print("BrackOS v1.2.0 Commands:\n");
        vga_print("  help       - This message\n");
        vga_print("  echo STR   - Print text\n");
        vga_print("  clear      - Clear screen\n");
        vga_print("  info       - System info\n");
        vga_print("  mem        - Memory layout\n");
        vga_print("  cpu        - CPU info\n");
        vga_print("  time       - Current time (HH:MM:SS)\n");
        vga_print("  date       - Current date (YYYY-MM-DD)\n");
        vga_print("  ver        - Version\n");
        vga_print("  uptime     - System uptime\n");
        vga_print("  credits    - Project credits\n");
        vga_print("  mouse      - Mouse info\n");
        vga_print("  fs         - Filesystem info\n");
        vga_print("  gui        - GUI mode\n");
        vga_print("  shutdown   - Halt\n");
        vga_print("  reboot     - Reboot\n");
        vga_print("  repeat N CMD - Run command N times\n");
        vga_print("  history    - Show recent commands (history clear)\n");
        vga_print("  heap       - Kernel heap statistics\n");
        vga_print("  pmm        - Physical memory (page frame) statistics\n");
        vga_print("  cache      - Disk cache statistics (cache clear)\n");
        vga_print("  crashtest X - Deliberately crash: null, stack, write, exec\n");
        vga_print("\n");
        vga_print("Filesystem commands (A: drive):\n");
        vga_print("  ls [PATH]  - List directory\n");
        vga_print("  cd DIR     - Change directory\n");
        vga_print("  mkdir NAME - Create directory\n");
        vga_print("  nf NAME    - Create new file\n");
        vga_print("  paper FILE - Edit file with paper\n");
        vga_print("  rm FILE    - Delete file\n");
        vga_print("  rmdir DIR  - Delete directory\n");
    }
    else if (strcmp(cmd, "history") == 0) {
        for (int i = 0; i < history_count(); i++) {
            vga_print("  ");
            vga_print_int(i + 1);
            vga_print("  ");
            vga_print(history_get(i));
            vga_print("\n");
        }
    }
    else if (strcmp(cmd, "history clear") == 0) {
        history_clear();
    }
    else if (strcmp(cmd, "heap") == 0) {
        heap_stats();
    }
    else if (strcmp(cmd, "pmm") == 0) {
        pmm_print_stats();
    }
    else if (strcmp(cmd, "cache") == 0) {
        uint32_t hits, misses, writes;
        fat32_cache_stats(&hits, &misses, &writes);
        vga_print("Disk sector cache (write-through, 256 sectors):\n  Read hits:   ");
        vga_print_int((int)hits);
        vga_print("\n  Read misses: ");
        vga_print_int((int)misses);
        vga_print("\n  Writes:      ");
        vga_print_int((int)writes);
        vga_print("\n");
    }
    else if (strcmp(cmd, "cache clear") == 0) {
        fat32_cache_clear();
        vga_print("Disk cache cleared\n");
    }
    else if (strncmp(cmd, "crashtest ", 10) == 0) {
        crash_test(&cmd[10]);
    }
    else if (strncmp(cmd, "echo ", 5) == 0) {
        vga_print(&cmd[5]);
        vga_print("\n");
    }
    else if (strcmp(cmd, "clear") == 0) {
        vga_clear();
    }
    else if (strcmp(cmd, "info") == 0) {
        vga_print("BrackOS v1.2.0 Information\n");
        vga_print("========================\n");
        vga_print("OS: BrackOS Experimental Kernel\n");
        vga_print("Arch: x86-64 (Long Mode)\n");
        vga_print("Boot: Multiboot2\n");
        vga_print("VGA: 80x25 text mode\n");
        vga_print("GUI: 1024x768@32bpp VBE\n");
        vga_print("IDT: INITIALIZED\n");
        vga_print("Interrupts: ENABLED (STI)\n");
        vga_print("Timer: IRQ0 (PIT @100Hz)\n");
        vga_print("RTC: CMOS enabled\n");
        vga_print("Filesystem: FAT32 (A: drive)\n");
        vga_print("Editor: Paper text editor\n");
        vga_print("Recovery: Enabled\n");
    }
    else if (strcmp(cmd, "mem") == 0) {
        meminfo_print();
    }
    else if (strcmp(cmd, "cpu") == 0) {
        cpu_print_info();
    }
    else if (strcmp(cmd, "time") == 0) {
        rtc_time_t now = rtc_get_time();
        print_time_component(now.hour);
        vga_print(":");
        print_time_component(now.minute);
        vga_print(":");
        print_time_component(now.second);
        vga_print("\n");
    }
    else if (strcmp(cmd, "date") == 0) {
        rtc_time_t now = rtc_get_time();
        vga_print_int(now.year);
        vga_print("-");
        print_time_component(now.month);
        vga_print("-");
        print_time_component(now.day);
        vga_print("\n");
    }
    else if (strcmp(cmd, "uptime") == 0) {
        uptime_t up = rtc_get_uptime();
        vga_print("Uptime: ");
        vga_print_int(up.days);
        vga_print(" days, ");
        vga_print_int(up.hours);
        vga_print(":");
        print_time_component(up.minutes);
        vga_print(":");
        print_time_component(up.seconds);
        vga_print("\n");
    }
    else if (strncmp(cmd, "sleep ", 6) == 0) {
        int seconds = 0;
        for (size_t i = 6; i < strlen(cmd); i++) {
            if (cmd[i] >= '0' && cmd[i] <= '9') {
                seconds = seconds * 10 + (cmd[i] - '0');
            }
        }
        if (seconds > 0) {
            vga_print("Sleeping ");
            vga_print_int(seconds);
            vga_print(" seconds...\n");
            timer_sleep_ms(seconds * 1000);
            vga_print("Done.\n");
        } else {
            vga_print("Usage: sleep <seconds>\n");
        }
    }
    else if (strcmp(cmd, "ver") == 0) {
        vga_print("BrackOS v1.2.0 (x86-64)\n");
        vga_print("Timer + RTC + Full Clock Functions\n");
    }
    else if (strcmp(cmd, "credits") == 0) {
        vga_print("BrackOS Credits:\n");
        vga_print("  Creator: BrackOS Team\n");
        vga_print("  Bootloader: GRUB 2.04\n");
        vga_print("  Tools: GCC, Binutils, NASM\n");
    }
    else if (strcmp(cmd, "mouse") == 0) {
        vga_print("Mouse Driver Status:\n");
        mouse_state_t state = *mouse_get();
        vga_print("  Position: (");
        vga_print_int(state.x);
        vga_print(", ");
        vga_print_int(state.y);
        vga_print(")\n");
        vga_print("  Buttons: L=");
        vga_print_int(state.left_pressed);
        vga_print(" R=");
        vga_print_int(state.right_pressed);
        vga_print(" M=");
        vga_print_int(state.middle_pressed);
        vga_print("\n");
    }
    else if (strcmp(cmd, "fs") == 0) {
        vga_print("Filesystem Status:\n");
        if (fat32_fs.boot.bytes_per_sector > 0) {
            vga_print("  Type: FAT32\n");
            vga_print("  Sector size: ");
            vga_print_int(fat32_fs.boot.bytes_per_sector);
            vga_print(" bytes\n");
            vga_print("  Cluster size: ");
            vga_print_int(fat32_fs.boot.sectors_per_cluster);
            vga_print(" sectors\n");
            vga_print("  Root cluster: ");
            vga_print_int(fat32_fs.root_dir_cluster);
            vga_print("\n");
        } else {
            vga_print("  Not initialized\n");
        }
    }
    else if (strcmp(cmd, "gui") == 0) {
        gui_mode = 1;
    }
    else if (strcmp(cmd, "shutdown") == 0) {
        interrupts_disable();
        vga_print("Halting CPU...\n");
        __asm__ volatile("cli; hlt");
    }
    else if (strcmp(cmd, "reboot") == 0) {
        interrupts_disable();
        vga_print("Rebooting...\n");
        for (int i = 0; i < 100; i++) {
            outb(0x64, 0xFE);
        }
        __asm__ volatile("cli; hlt");
    }
    /* Filesystem commands */
    else if (strcmp(cmd, "ls") == 0) {
        fs_ls(0);
    }
    else if (strncmp(cmd, "ls ", 3) == 0) {
        fs_ls(&cmd[3]);
    }
    else if (strcmp(cmd, "cd /") == 0 || strcmp(cmd, "cd A:/") == 0) {
        fs_cd("/");
    }
    else if (strncmp(cmd, "cd ", 3) == 0) {
        fs_cd(&cmd[3]);
    }
    else if (strncmp(cmd, "mkdir ", 6) == 0) {
        fs_mkdir(&cmd[6]);
    }
    else if (strncmp(cmd, "nf ", 3) == 0) {
        fs_nf(&cmd[3]);
    }
    else if (strncmp(cmd, "rm ", 3) == 0) {
        fs_rm(&cmd[3]);
    }
    else if (strncmp(cmd, "rmdir ", 6) == 0) {
        fs_rmdir(&cmd[6]);
    }
    else if (strncmp(cmd, "paper ", 6) == 0) {
        paper_open_file(&cmd[6], current_dir.cluster);
    }
    else if (strncmp(cmd, "repeat ", 7) == 0) {
        int count = 0;
        int i = 7;
        while (cmd[i] >= '0' && cmd[i] <= '9') {
            count = count * 10 + (cmd[i] - '0');
            i++;
        }
        while (i < (int)strlen(cmd) && cmd[i] == ' ') i++;
        
        if (count > 0 && i < (int)strlen(cmd)) {
            char *subcmd = &cmd[i];
            for (int r = 0; r < count; r++) {
                process_command(subcmd);
            }
        }
    }
    else {
        vga_print("Unknown command: ");
        vga_print(cmd);
        vga_print("\n");
    }
}

void gui_mode_loop(void) {
    fb_init();
    font_init();
    window_init();
    mouse_init();
    
    fb_fill_rect(0, 0, 1024, 768, 0x78ACD4);
    fb_fill_rect(0, 728, 1024, 40, 0xFFFFFF);
    
    window_t info_win = {
        .x = 50, .y = 50, .width = 400, .height = 200,
        .title = "System Info", .visible = 1, .z_order = 0
    };
    window_init_buffer(&info_win);
    window_draw(&info_win);
    
    vga_print("GUI mode: Press Q to exit\n");
    
    while (1) {
        mouse_update();
        cursor_draw(mouse_get()->x, mouse_get()->y);
        
        if (keyboard_has_input()) {
            uint8_t scancode = inb(0x60);
            if (scancode == 0x10) {
                break;
            }
        }
    }
    
    gui_mode = 0;
    vga_clear();
}

void shell_loop(void) {
    vga_clear();
    vga_set_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK);
    vga_print("BrackOS v1.2.0 Shell\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("Type 'help' for commands\n\n");
    
    while (1) {
        char path_display[256];
        fs_get_cwd(path_display);
        vga_print(path_display);
        vga_print("> ");
        
        /* Line editor with cursor movement and command history */
        readline(cmd_buffer, CMD_BUFFER_SIZE);
        cmd_pos = (int)strlen(cmd_buffer);
        process_command(cmd_buffer);

        if (gui_mode) {
            gui_mode_loop();
        }
    }
}

/* IRQ0 handler - installed by IDT */
void irq0_handler(void) {
    timer_irq_handler();
}

/* Deliberate crashes to check the memory protections (each halts the system) */
static volatile int crash_dummy;
static volatile int crash_limit = -1;      /* never reached: keeps the recursion honest */
static uint8_t crash_data[64];

static int crash_recurse(int depth) {
    volatile char pad[512];
    pad[0] = (char)depth;
    if (depth == crash_limit) return 0;
    return crash_recurse(depth + 1) + pad[0];
}

static void crash_test(const char *what) {
    if (strcmp(what, "null") == 0) {
        crash_dummy = *(volatile int *)0;                 /* read page 0 */
    } else if (strcmp(what, "stack") == 0) {
        crash_dummy = crash_recurse(0);                   /* run off the stack */
    } else if (strcmp(what, "write") == 0) {
        *(volatile uint8_t *)(uintptr_t)crash_test = 0;   /* write to code */
    } else if (strcmp(what, "exec") == 0) {
        ((void (*)(void))(uintptr_t)crash_data)();        /* run data */
    } else {
        vga_print("Usage: crashtest null|stack|write|exec\n");
    }
}

void kmain(uint32_t mbi_addr) {
    serial_init();
    multiboot_init(mbi_addr);
    vga_init();

    /* Protection first: our own GDT/TSS (crash stacks), then tighter page
     * tables (null page, stack guard, W^X). The new GDT has to be live
     * before paging makes the boot-time GDT in the code section read-only. */
    gdt_init();
    paging_init();

    /* Physical memory from the Multiboot2 map, then a heap carved out of it */
    pmm_init(mbi_addr);
    {
        uint32_t heap_frames = 4096;             /* try 16 MB, then 4 MB, then 1 MB */
        uint64_t heap_phys = 0;
        while (heap_frames >= 256) {
            heap_phys = pmm_alloc_frames(heap_frames);
            if (heap_phys) break;
            heap_frames /= 4;
        }
        if (heap_phys) {
            heap_init((void *)(uintptr_t)heap_phys, (size_t)heap_frames * PMM_FRAME_SIZE);
        } else {
            vga_print("WARNING: no memory available for the kernel heap\n");
        }
    }
    keyboard_init();
    idt_init();
    interrupts_enable();
    
    /* Initialize timer and RTC */
    timer_init();
    pic_enable_irq(0); /* Enable timer interrupt */
    rtc_init();
    
    recovery_init();
    
    if (fat32_init(0) != 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_print("Warning: FAT32 init failed (no disk?)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
    
    if (recovery_get_mode() == RECOVERY_SAFE_MODE) {
        recovery_safe_mode();
    }
    
    fs_shell_init();
    shell_loop();
    
    __asm__ volatile("cli; hlt");
}
