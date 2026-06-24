#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts          */
    outb(COM1 + 3, 0x80);   /* DLAB on                     */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud    */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1                         */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear              */
    outb(COM1 + 4, 0x0B);   /* IRQs on, RTS/DSR            */
}

void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) { }
    outb(COM1, (uint8_t)c);
    outb(0xE9, (uint8_t)c);
}
