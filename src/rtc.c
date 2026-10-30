#include "rtc.h"
#include "io.h"
#include "vga.h"
#include "string.h"

uptime_t system_uptime = {0, 0, 0, 0};
static rtc_time_t last_time = {0};
static uint8_t rtc_initialized = 0;

/* Read byte from CMOS */
static uint8_t cmos_read(uint8_t addr) {
    outb(RTC_ADDR_PORT, addr);
    return inb(RTC_DATA_PORT);
}

/* Write byte to CMOS */
static void cmos_write(uint8_t addr, uint8_t value) {
    outb(RTC_ADDR_PORT, addr);
    outb(RTC_DATA_PORT, value);
}

/* BCD to Binary */
uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

/* Binary to BCD */
uint8_t bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}

/* Check if RTC is updating */
int rtc_is_updating(void) {
    return (cmos_read(RTC_STATUS_A) & 0x80) != 0;
}

/* Initialize RTC */
void rtc_init(void) {
    if (rtc_initialized) return;
    
    vga_print("RTC: Initializing...\n");
    
    /* Read initial time (rtc_read_time waits for any update to finish) */
    rtc_read_time(&last_time);
    
    vga_print("RTC: ");
    vga_print_int(last_time.year);
    vga_print("-");
    vga_print_int(last_time.month);
    vga_print("-");
    vga_print_int(last_time.day);
    vga_print(" ");
    vga_print_int(last_time.hour);
    vga_print(":");
    vga_print_int(last_time.minute);
    vga_print(":");
    vga_print_int(last_time.second);
    vga_print("\n");
    
    rtc_initialized = 1;
}

/* Enable RTC periodic interrupt */
void rtc_enable_irq(void) {
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    cmos_write(RTC_STATUS_B, status_b | 0x40); /* Enable periodic interrupt */
}

/* Read current time from RTC */
rtc_time_t rtc_get_time(void) {
    rtc_time_t time;
    rtc_read_time(&time);
    return time;
}

/* Raw register values exactly as the chip reports them */
typedef struct {
    uint8_t second, minute, hour, day_of_week, day, month, year, century;
} rtc_raw_t;

#define RTC_CENTURY 0x32   /* common (not universal) century register */

/* Wait for the "update in progress" flag to clear (bounded, so a dead RTC
 * cannot hang the kernel) */
static void rtc_wait_ready(void) {
    for (int t = 0; t < 1000000 && rtc_is_updating(); t++) { }
}

static void rtc_read_raw(rtc_raw_t *r) {
    rtc_wait_ready();
    r->second      = cmos_read(RTC_SECONDS);
    r->minute      = cmos_read(RTC_MINUTES);
    r->hour        = cmos_read(RTC_HOURS);
    r->day_of_week = cmos_read(RTC_DAY_OF_WEEK);
    r->day         = cmos_read(RTC_DAY_OF_MONTH);
    r->month       = cmos_read(RTC_MONTH);
    r->year        = cmos_read(RTC_YEAR);
    r->century     = cmos_read(RTC_CENTURY);
}

/* Read time with BCD / 12-hour conversion */
void rtc_read_time(rtc_time_t *time) {
    /* Read twice until two reads agree, so we never catch the chip
     * halfway through a rollover (e.g. 10:59:59 -> 11:00:00) */
    rtc_raw_t cur, prev;
    rtc_read_raw(&cur);
    for (int tries = 0; tries < 5; tries++) {
        prev = cur;
        rtc_read_raw(&cur);
        if (memcmp(&cur, &prev, sizeof(cur)) == 0) break;
    }

    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int is_binary = (status_b & 0x04) != 0;
    int is_24h = (status_b & 0x02) != 0;

    /* In 12-hour mode bit 7 of the hour register is the PM flag. It must be
     * read BEFORE the BCD conversion, which would otherwise discard it. */
    int pm = (cur.hour & 0x80) != 0;
    uint8_t hour = cur.hour & 0x7F;

    uint8_t second = cur.second, minute = cur.minute;
    uint8_t day = cur.day, month = cur.month, year = cur.year;
    uint8_t dow = cur.day_of_week, century = cur.century;

    if (!is_binary) {
        second  = bcd_to_bin(second);
        minute  = bcd_to_bin(minute);
        hour    = bcd_to_bin(hour);
        day     = bcd_to_bin(day);
        month   = bcd_to_bin(month);
        year    = bcd_to_bin(year);
        dow     = bcd_to_bin(dow);
        century = bcd_to_bin(century);
    }

    /* 12-hour clock: 12 AM = 0, 1-11 AM = 1-11, 12 PM = 12, 1-11 PM = 13-23 */
    if (!is_24h) {
        hour = hour % 12;
        if (pm) hour += 12;
    }

    /* Century register is missing or garbage on some machines: assume 20xx */
    if (century < 19 || century > 21) century = 20;

    time->second = second;
    time->minute = minute;
    time->hour = hour;
    time->day_of_week = dow;
    time->day = day;
    time->month = month;
    time->year = (uint16_t)(century * 100 + year);
}

/* Update system uptime (called periodically by timer IRQ) */
void rtc_update_uptime(void) {
    system_uptime.seconds++;
    
    if (system_uptime.seconds >= 60) {
        system_uptime.seconds = 0;
        system_uptime.minutes++;
    }
    
    if (system_uptime.minutes >= 60) {
        system_uptime.minutes = 0;
        system_uptime.hours++;
    }
    
    if (system_uptime.hours >= 24) {
        system_uptime.hours = 0;
        system_uptime.days++;
    }
}

/* Get system uptime */
uptime_t rtc_get_uptime(void) {
    return system_uptime;
}
