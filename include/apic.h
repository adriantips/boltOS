#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Local APIC + IO-APIC + APIC timer + MSI.
 *
 *  apic_init() masks the legacy 8259 PICs, enables the local APIC, routes the
 *  legacy ISA IRQs we care about (keyboard, mouse, COM, ATA) through the
 *  IO-APIC, and calibrates the local-APIC timer against the PIT so the
 *  preemptive scheduler ticks on APIC vector 32 (same vector the PIT used, so
 *  the existing irq0 handler + scheduler hook keep working).
 * ===========================================================================*/

int  apic_init(void);            /* 0 on success, <0 if no APIC */
int  apic_active(void);          /* 1 once the APIC owns interrupt delivery */
void lapic_eoi(void);            /* end-of-interrupt to the local APIC */
void lapic_enable_ap(void);      /* enable the calling AP's local APIC */
uint8_t lapic_id(void);          /* this CPU's local APIC id */

/* Route a legacy ISA IRQ (0..15) to an IDT vector on the boot CPU via the
 * IO-APIC, unmasked. Applies MADT interrupt-source overrides. */
void ioapic_route_irq(uint8_t irq, uint8_t vector);
void ioapic_mask_irq(uint8_t irq, int masked);

/* Inter-processor interrupts (used by the SMP bring-up + TLB shootdown). */
void lapic_send_init(uint8_t apic_id);
void lapic_send_sipi(uint8_t apic_id, uint8_t vector);
void lapic_send_ipi(uint8_t apic_id, uint8_t vector);
void lapic_broadcast_ipi(uint8_t vector);     /* all-excluding-self */

/* Compose an MSI (address,data) pair targeting the boot CPU + given vector.
 * Edge-triggered, fixed delivery. Lets PCI devices raise MSI instead of a
 * shared legacy line. */
void apic_msi_compose(uint8_t vector, uint32_t *addr_lo, uint32_t *data);

/* Local-APIC timer period in nanoseconds-per-tick, for reference. */
uint64_t lapic_timer_hz(void);
