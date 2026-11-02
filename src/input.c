#include "input.h"
#include "keyboard.h"
#include <stdint.h>

#define QUEUE_SIZE 128                   /* power of two */

static input_event_t queue[QUEUE_SIZE];
static unsigned head = 0;                /* next event to read */
static unsigned tail = 0;                /* next free slot */
static uint8_t last_buttons = 0;
static volatile int interrupt_flag = 0;     /* Ctrl-C was pressed */

/* Briefly turn interrupts off so the queue can be shared with the handlers */
static inline uintptr_t irq_save(void) {
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uintptr_t flags) {
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
}

static inline unsigned count(void) { return (tail - head) & (QUEUE_SIZE - 1); }

void input_init(void) {
    uintptr_t f = irq_save();
    head = tail = 0;
    last_buttons = 0;
    irq_restore(f);
}

/* Caller has interrupts off. If the queue is full the oldest event is dropped. */
static void push_locked(const input_event_t *ev) {
    if (count() == QUEUE_SIZE - 1) head = (head + 1) & (QUEUE_SIZE - 1);
    queue[tail] = *ev;
    tail = (tail + 1) & (QUEUE_SIZE - 1);
}

void input_push_key(char c) {
    if (c == 3) { interrupt_flag = 1; return; }   /* Ctrl-C: not a character, a request to stop */
    input_event_t ev = { .type = INPUT_KEY, .key = c };
    uintptr_t f = irq_save();
    push_locked(&ev);
    irq_restore(f);
}

void input_push_mouse(int x, int y, int dx, int dy, uint8_t buttons) {
    uintptr_t f = irq_save();

    if (buttons != last_buttons) {
        input_event_t ev = { .type = INPUT_MOUSE_BUTTON, .buttons = buttons,
                             .changed = (uint8_t)(buttons ^ last_buttons), .x = x, .y = y };
        last_buttons = buttons;
        push_locked(&ev);
    }

    if (dx != 0 || dy != 0) {
        /* Merge into the previous move event so a fast mouse cannot flood the queue */
        unsigned prev = (tail - 1) & (QUEUE_SIZE - 1);
        if (count() > 0 && queue[prev].type == INPUT_MOUSE_MOVE) {
            queue[prev].x = x;
            queue[prev].y = y;
            queue[prev].dx += dx;
            queue[prev].dy += dy;
            queue[prev].buttons = buttons;
        } else {
            input_event_t ev = { .type = INPUT_MOUSE_MOVE, .buttons = buttons,
                                 .x = x, .y = y, .dx = dx, .dy = dy };
            push_locked(&ev);
        }
    }
    irq_restore(f);
}

int input_poll(input_event_t *ev) {
    keyboard_poll();                     /* no-op once the keyboard interrupt is on */
    uintptr_t f = irq_save();
    int got = 0;
    if (count() > 0) {
        *ev = queue[head];
        head = (head + 1) & (QUEUE_SIZE - 1);
        got = 1;
    }
    irq_restore(f);
    return got;
}

int input_key_pending(void) {
    keyboard_poll();
    uintptr_t f = irq_save();
    int found = 0;
    for (unsigned i = head; i != tail; i = (i + 1) & (QUEUE_SIZE - 1)) {
        if (queue[i].type == INPUT_KEY) { found = 1; break; }
    }
    irq_restore(f);
    return found;
}

void input_wait(input_event_t *ev) {
    for (;;) {
        keyboard_poll();
        uintptr_t f = irq_save();
        if (count() > 0) {
            *ev = queue[head];
            head = (head + 1) & (QUEUE_SIZE - 1);
            irq_restore(f);
            return;
        }
        if ((f & 0x200) && keyboard_irq_active()) {
            /* Nothing to do: sleep until an interrupt. "sti; hlt" is atomic, so
             * an event that arrives right now wakes us instead of being missed. */
            __asm__ volatile("sti; hlt" : : : "memory");
        } else {
            irq_restore(f);
            __asm__ volatile("pause");   /* interrupts off or not set up yet: poll */
        }
    }
}

/* Was Ctrl-C pressed since the last call? */
int input_take_interrupt(void) {
    int was = interrupt_flag;
    interrupt_flag = 0;
    return was;
}
