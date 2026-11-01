//
// Secondary-core entry and ACPI parking-protocol loop (Exynos5420).
//
// The secure firmware releases a freshly powered A15 core (cpu state flag
// SECONDARY_RESET in SYSRAM_NS + 0x28) to the address in SYSRAM_NS + 0x1C:
// non-secure SVC, ARM state, MMU and caches off.  MpParkDxe copies this
// code into reserved memory, patches the literals at MpParkLiterals and
// points SYSRAM_NS + 0x1C here.
//
// Each core finds its 4 KB mailbox at MailboxBase + (MPIDR.Aff0 << 12):
//   0x000  ProcessorId   (OS: GIC CPU interface number of the target)
//   0x008  JumpAddress   (OS, 64-bit)
//   0x800  firmware area (below), status for MpParkDxe
//
// The core enables its GIC CPU interface so the OS's wakeup SGI ends WFI.
// Only priority 0 interrupts get through (PMR 0x10, SGI priority 0 on this
// core): a peripheral interrupt the OS later routes here while the core is
// still parked is left alone rather than acknowledged and lost.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#define MB_PROCESSOR_ID  0x000
#define MB_JUMP_ADDRESS  0x008
#define FW_STATE         0x800    // 1 parked, 2 released to the OS
#define FW_GIC_ID        0x804    // own GIC CPU interface number
#define FW_ACTLR         0x808
#define FW_CPSR          0x80C
#define FW_MPIDR         0x810
#define FW_SCTLR         0x814

#define GICC_CTLR        0x000
#define GICC_PMR         0x004
#define GICC_IAR         0x00C
#define GICC_EOIR        0x010
#define GICD_ISENABLER0  0x100
#define GICD_IPRIORITYR0 0x400
#define GICD_ITARGETSR0  0x800

  .text
  .arm
  .align 5

  .global MpParkStart
  .global MpParkLiterals
  .global MpParkEnd

MpParkStart:
  cpsid   aif
  mrc     p15, 0, r4, c0, c0, 5         // MPIDR
  and     r5, r4, #3                    // core within the cluster
  adr     r6, MpParkLiterals
  ldr     r7, [r6, #0]                  // mailbox base
  ldr     r8, [r6, #4]                  // GICC
  ldr     r9, [r6, #8]                  // GICD
  add     r7, r7, r5, lsl #12           // this core's mailbox

  str     r4, [r7, #FW_MPIDR]
  mrs     r0, cpsr
  str     r0, [r7, #FW_CPSR]
  mrc     p15, 0, r0, c1, c0, 1
  str     r0, [r7, #FW_ACTLR]
  mrc     p15, 0, r0, c1, c0, 0
  str     r0, [r7, #FW_SCTLR]

  //
  // Own GIC CPU interface number: ITARGETSR0 is banked, each byte reads as
  // this interface's mask.
  //
  ldr     r0, [r9, #GICD_ITARGETSR0]
  and     r0, r0, #0xFF
  clz     r0, r0
  rsb     r10, r0, #31
  str     r10, [r7, #FW_GIC_ID]

  //
  // SGIs: enabled, priority 0, and nothing else unmasked.
  //
  mov     r0, #0
  str     r0, [r9, #(GICD_IPRIORITYR0 + 0x0)]
  str     r0, [r9, #(GICD_IPRIORITYR0 + 0x4)]
  str     r0, [r9, #(GICD_IPRIORITYR0 + 0x8)]
  str     r0, [r9, #(GICD_IPRIORITYR0 + 0xC)]
  movw    r0, #0xFFFF
  str     r0, [r9, #GICD_ISENABLER0]
  mov     r0, #0x10
  str     r0, [r8, #GICC_PMR]
  mov     r0, #1
  str     r0, [r8, #GICC_CTLR]

  mov     r0, #1
  str     r0, [r7, #FW_STATE]
  dsb

Park:
  ldr     r0, [r7, #MB_PROCESSOR_ID]
  cmp     r0, r10
  beq     Release
  wfi
  ldr     r0, [r8, #GICC_IAR]
  movw    r1, #0x3FF
  and     r2, r0, r1
  cmp     r2, r1
  strne   r0, [r8, #GICC_EOIR]          // ack + EOI the wakeup SGI
  b       Park

Release:
  dmb                                   // ProcessorId before JumpAddress
  ldr     r11, [r7, #MB_JUMP_ADDRESS]
  cmp     r11, #0
  beq     Park
  mov     r0, #0
  str     r0, [r7, #MB_JUMP_ADDRESS]
  str     r0, [r7, #(MB_JUMP_ADDRESS + 4)]
  mov     r0, #2
  str     r0, [r7, #FW_STATE]
  dsb
  isb
  mov     r0, r7                        // mailbox address, for whoever wants it
  bx      r11

  .align 2
MpParkLiterals:
  .long   0                             // mailbox base
  .long   0                             // GICC base
  .long   0                             // GICD base
MpParkEnd:
