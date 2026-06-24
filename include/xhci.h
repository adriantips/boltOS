#pragma once

/* Bring up the first xHCI USB 3.x host controller (PCI class 0Ch sub 03h
 * prog-if 30h), enumerate attached devices, and report them. Polled, no
 * interrupts. Safe no-op if no controller is present. */
void xhci_init(void);
