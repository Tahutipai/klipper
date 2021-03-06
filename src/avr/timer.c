// AVR timer interrupt scheduling code.
//
// Copyright (C) 2016  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <avr/interrupt.h> // TCNT1
#include "autoconf.h" // CONFIG_AVR_CLKPR
#include "board/misc.h" // timer_from_us
#include "command.h" // shutdown
#include "irq.h" // irq_save
#include "sched.h" // sched_timer_kick


/****************************************************************
 * Low level timer code
 ****************************************************************/

DECL_CONSTANT(CLOCK_FREQ, CONFIG_CLOCK_FREQ);

// Return the number of clock ticks for a given number of microseconds
uint32_t
timer_from_us(uint32_t us)
{
    return us * (CONFIG_CLOCK_FREQ / 1000000);
}

union u32_u {
    struct { uint8_t b0, b1, b2, b3; };
    struct { uint16_t lo, hi; };
    uint32_t val;
};

// Return true if time1 is before time2.  Always use this function to
// compare times as regular C comparisons can fail if the counter
// rolls over.
uint8_t __always_inline
timer_is_before(uint32_t time1, uint32_t time2)
{
    // This asm is equivalent to:
    //     return (int32_t)(time1 - time2) < 0;
    // But gcc doesn't do a good job with the above, so it's hand coded.
    union u32_u utime1 = { .val = time1 };
    uint8_t f = utime1.b3;
    asm("    cp  %A1, %A2\n"
        "    cpc %B1, %B2\n"
        "    cpc %C1, %C2\n"
        "    sbc %0,  %D2"
        : "+r"(f) : "r"(time1), "r"(time2));
    return (int8_t)f < 0;
}

static inline uint16_t
timer_get(void)
{
    return TCNT1;
}

static inline void
timer_set(uint16_t next)
{
    OCR1A = next;
}

static inline void
timer_repeat_set(uint16_t next)
{
    // Timer1B is used to limit the number of timers run from a timer1A irq
    OCR1B = next;
    TIFR1 = 1<<OCF1B;
}

// Reset the timer - clear settings and dispatch next timer immediately
static void
timer_reset(void)
{
    uint16_t now = timer_get();
    timer_set(now + 50);
    TIFR1 = 1<<OCF1A;
    timer_repeat_set(now + 50);
}
DECL_SHUTDOWN(timer_reset);

static void
timer_init(void)
{
    if (CONFIG_AVR_CLKPR != -1 && (uint8_t)CONFIG_AVR_CLKPR != CLKPR) {
        // Program the clock prescaler
        irqstatus_t flag = irq_save();
        CLKPR = 0x80;
        CLKPR = CONFIG_AVR_CLKPR;
        irq_restore(flag);
    }

    // no outputs
    TCCR1A = 0;
    // Normal Mode
    TCCR1B = 1<<CS10;
    // Setup for first irq
    irqstatus_t flag = irq_save();
    timer_reset();
    TIFR1 = 1<<TOV1;
    // enable interrupt
    TIMSK1 = 1<<OCIE1A;
    irq_restore(flag);
}
DECL_INIT(timer_init);


/****************************************************************
 * 32bit timer wrappers
 ****************************************************************/

static uint16_t timer_high;

// Return the current time (in absolute clock ticks).
uint32_t
timer_read_time(void)
{
    irqstatus_t flag = irq_save();
    union u32_u calc;
    calc.val = timer_get();
    calc.hi = timer_high;
    if (TIFR1 & (1<<TOV1) && calc.lo < 0x8000)
        calc.hi++;
    irq_restore(flag);
    return calc.val;
}

// Called by main code once every millisecond.  (IRQs disabled.)
void
timer_periodic(void)
{
    if (TIFR1 & (1<<TOV1)) {
        // Hardware timer has overflowed - update overflow counter
        TIFR1 = 1<<TOV1;
        timer_high++;
    }
}

#define TIMER_IDLE_REPEAT_TICKS 8000
#define TIMER_REPEAT_TICKS 3000

#define TIMER_MIN_TRY_TICKS 60 // 40 ticks to exit irq; 20 ticks of progress
#define TIMER_DEFER_REPEAT_TICKS 200

// Hardware timer IRQ handler - dispatch software timers
ISR(TIMER1_COMPA_vect)
{
    uint16_t next;
    for (;;) {
        // Run the next software timer
        next = sched_timer_dispatch();

        int16_t diff = timer_get() - next;
        if (likely(diff >= 0)) {
            // Another timer is pending - briefly allow irqs to fire
            irq_enable();
            if (unlikely(TIFR1 & (1<<OCF1B)))
                // Too many repeat timers - must exit irq handler
                goto force_defer;
            irq_disable();
            continue;
        }

        if (likely(diff <= -TIMER_MIN_TRY_TICKS))
            // Schedule next timer normally
            goto done;

        // Next timer in very near future - wait for it to be ready
        do {
            irq_enable();
            if (unlikely(TIFR1 & (1<<OCF1B)))
                goto force_defer;
            irq_disable();
            diff = timer_get() - next;
        } while (diff < 0);
    }

force_defer:
    // Too many repeat timers - force a pause so tasks aren't starved
    irq_disable();
    uint16_t now = timer_get();
    if ((int16_t)(next - now) < (int16_t)(-timer_from_us(1000)))
        shutdown("Rescheduled timer in the past");
    timer_repeat_set(now + TIMER_REPEAT_TICKS);
    next = now + TIMER_DEFER_REPEAT_TICKS;

done:
    timer_set(next);
    return;
}

// Periodic background task that temporarily boosts priority of
// timers.  This helps prioritize timers when tasks are idling.
static void
timer_task(void)
{
    irq_disable();
    timer_repeat_set(timer_get() + TIMER_IDLE_REPEAT_TICKS);
    irq_enable();
}
DECL_TASK(timer_task);
