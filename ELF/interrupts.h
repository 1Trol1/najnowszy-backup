#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

/* 64‑bitowy wpis IDT */
struct idt_entry64 {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

/* Struktura dla LIDT */
struct idt_ptr64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Ramka przerwania dla __attribute__((interrupt)) w SysV ABI */
struct interrupt_frame {
    uint64_t rip;
    uint16_t cs;
    uint16_t _pad0;
    uint32_t _pad1;
    uint64_t rflags;
    uint64_t rsp;
    uint16_t ss;
    uint16_t _pad2;
    uint32_t _pad3;
};

/* Statyczne IDT + IDTR (header‑only => static) */
static struct idt_entry64 idt[256];
static struct idt_ptr64   idtr;

/* Ustawienie bramki IDT */
static inline void idt_set_gate(uint8_t vec, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;

    idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector    = 0x08;       /* zakładamy GDT z kodem pod 0x08 */
    idt[vec].ist         = 0;
    idt[vec].type_attr   = 0x8E;       /* present, DPL=0, interrupt gate */
    idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero        = 0;
}

/* LIDT w inline asm (SysV ABI – normalne wywołanie funkcji) */
static inline void idt_load(void) {
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint64_t)&idt[0];

    __asm__ __volatile__("lidt %0" : : "m"(idtr));
}

static void isr_divide_by_zero(struct interrupt_frame *frame) {
    (void)frame;
    for (;;) __asm__ __volatile__("hlt");
}

static void isr_general_protection(struct interrupt_frame *frame,
                                   uint64_t error_code) {
    (void)frame;
    (void)error_code;
    for (;;) __asm__ __volatile__("hlt");
}

static inline void interrupts_init(void) {
    for (int i = 0; i < 256; ++i) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].zero        = 0;
    }

    idt_set_gate(0,  (void (*)(void))isr_divide_by_zero);
    // jeśli chcesz, możesz na razie zakomentować GPF:
    // idt_set_gate(13, (void (*)(void))isr_general_protection);

    idt_load();
    __asm__ __volatile__("sti");
}

#endif /* INTERRUPTS_H */
