//
//  kern_platprobe.cpp
//  UnifiedRed (from KaberiRed)
//
//  Platform probe (-crplatprobe). On the HP A4-5000 test laptop, Monterey froze completely (keyboard dead, no panic)
//  a few seconds into every boot, at a different log line each time, whatever kexts were loaded (2026-10-17,
//  docs/gcn2/TEST_MACHINES.md). Before the freeze, 1 ms sleeps were taking ~15 ms. This prints, every 250 ms, what the
//  platform is doing, so the last lines on screen before a freeze show what changed:
//
//    CRP <n> t=<ms> d=<mach>/<hpet>/<pmtmr> ms  irr=<vectors> isr=<vectors> tpr=<x> lvtt=<x>/<init>/<cur>
//        pm1=<sts>/<en> gpe=<sts>/<en> hpet=<conf>/<intsts> ioapic=<pins>
//
//  - d: milliseconds since the previous line by three independent clocks: mach_absolute_time (the TSC), the HPET main
//    counter and the ACPI PM timer. They should all read ~250; a lagging TSC, a stopped HPET or a starved thread
//    (all three large) show up here.
//  - irr/isr: vectors pending / in service at this CPU's local APIC. A storm keeps the same vector pending.
//  - lvtt: the local APIC timer's LVT entry, initial and current count (macOS's timer).
//  - pm1/gpe: ACPI fixed-event and GPE0 status/enable (FADT: PM1a 0x400, GPE0 0x420, 8 bytes): a status bit that
//    stays set while enabled is an event nobody clears.
//  - hpet: general configuration and interrupt status, then each timer's configuration if its interrupt is enabled.
//  - ioapic: (one CPU only) I/O APIC 0 pins that are unmasked, as <pin>:<vector><L if level><R if remote IRR set>.
//    A level-triggered pin stuck with remote IRR is an interrupt that was delivered and never acknowledged.
//  - cpu: the local APIC ID of the CPU the line was taken on.
//  - smm: gaps in the wait: <count>/<total us>/<longest us> (below).
//  - idle: what machine_idle does: "hlt" (sti; hlt), "nop" (the EFI builder's --nohlt patch landed), "nosym"
//    (Lilu couldn't solve _machine_idle, as in Recovery on 2026-11-01) or "?" (no such instruction found).
//
//  The probe waits by spinning, not sleeping. With IOSleep it stopped at the freeze along with every other sleeping
//  thread (the timer interrupt stops arriving; 2026-10-17 and 2026-11-01), so it couldn't show the freeze itself;
//  spinning keeps it printing as long as its CPU runs. It spins in 1 ms slices with interrupts off and counts gaps of
//  over 20 us between consecutive clock reads: with interrupts off, that is time the CPU spent where the OS can't see
//  (SMM, the firmware). Interrupts are on between slices, so the thread can still be preempted.
//
//  Everything is read-only. Register layouts: Intel SDM vol. 3 ch. 10 (local APIC), IA-PC HPET 1.0a, ACPI 6.x
//  (PM1/GPE blocks), 82093AA I/O APIC datasheet. The I/O ports and addresses are this laptop's (FADT, MADT, HPET table).
//

#include "kern_platprobe.hpp"

#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

namespace {
	constexpr uint64_t LapicBase = 0xFEE00000, HpetBase = 0xFED00000, IoapicBase = 0xFEC00000;
	constexpr uint16_t Pm1Status = 0x400, Pm1Enable = 0x402, PmTimer = 0x408, Gpe0Status = 0x420, Gpe0Enable = 0x424;
	constexpr uint32_t TickMs = 250, RunSeconds = 120;
	constexpr uint64_t SliceNs = 1000000, GapNs = 20000;

	const char *idleState = "?";

	inline uint16_t inw(uint16_t port) {
		uint16_t v;
		asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
		return v;
	}
	inline uint32_t inl(uint16_t port) {
		uint32_t v;
		asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
		return v;
	}

	IOMemoryMap *mapPhys(uint64_t phys, uint64_t size) {
		auto *desc = IOMemoryDescriptor::withPhysicalAddress(phys, size, kIODirectionIn);
		auto *map = desc ? desc->map(kIOMapInhibitCache) : nullptr;
		OSSafeReleaseNULL(desc);
		return map;
	}

	// The set bits of a 256-bit local APIC vector register (8 dwords, 0x10 apart), as "v,v,v" (at most 6 shown).
	void vectors(volatile uint32_t *lapic, uint32_t offset, char *out, size_t size) {
		size_t len = 0;
		uint32_t shown = 0, total = 0;
		out[0] = '-';
		out[1] = '\0';
		for (uint32_t i = 0; i < 8; i++) {
			uint32_t bits = lapic[(offset + 0x10 * i) / 4];
			for (uint32_t b = 0; b < 32; b++) {
				if (!(bits & (1U << b)))
					continue;
				total++;
				if (shown++ < 6 && len + 6 < size)
					len += static_cast<size_t>(snprintf(out + len, size - len, "%s%x", len ? "," : "", i * 32 + b));
			}
		}
		if (total > 6 && len + 8 < size)
			snprintf(out + len, size - len, "+%u", total - 6);
	}

	void probeThread(void *, wait_result_t) {
		auto *lapicMap = mapPhys(LapicBase, 0x1000);
		auto *hpetMap = mapPhys(HpetBase, 0x400);
		auto *ioapicMap = mapPhys(IoapicBase, 0x1000);
		auto *lapic = lapicMap ? reinterpret_cast<volatile uint32_t *>(lapicMap->getVirtualAddress()) : nullptr;
		auto *hpet = hpetMap ? reinterpret_cast<volatile uint64_t *>(hpetMap->getVirtualAddress()) : nullptr;
		auto *ioapic = ioapicMap ? reinterpret_cast<volatile uint32_t *>(ioapicMap->getVirtualAddress()) : nullptr;
		// HPET counter period in femtoseconds (GCAP_ID[63:32]); timers in this probe use it to convert ticks to ms.
		uint64_t hpetPeriodFs = hpet ? hpet[0] >> 32 : 0;
		// The I/O APIC is read through an index register, safe only with one CPU (the cpus=1 boot-arg).
		uint32_t cpus = 0;
		bool oneCpu = PE_parse_boot_argn("cpus", &cpus, sizeof(cpus)) && cpus == 1;
		uint32_t panicAt = 0;
		PE_parse_boot_argn("crprobepanic", &panicAt, sizeof(panicAt));
		IOLog("CRP: start (spin wait): lapic %d hpet %d (period %llu fs, %u timers) ioapic %d (%s); pmtmr %08x\n",
			lapic != nullptr, hpet != nullptr, hpetPeriodFs, hpet ? static_cast<uint32_t>(((hpet[0] >> 8) & 0x1F) + 1) : 0,
			ioapic != nullptr, oneCpu ? "read" : "skipped: more than one CPU", inl(PmTimer));

		uint64_t lastMach = mach_absolute_time(), lastHpet = hpet ? hpet[0xF0 / 8] : 0;
		uint32_t lastPm = inl(PmTimer);
		uint64_t startMach = lastMach;
		for (uint32_t n = 0; n < RunSeconds * 1000 / TickMs; n++) {
			// mach_absolute_time is in nanoseconds on x86_64.
			uint64_t deadline = lastMach + TickMs * 1000000ULL, gapCount = 0, gapSum = 0, gapMax = 0;
			for (uint64_t now = mach_absolute_time(); now < deadline;) {
				bool enabled = ml_set_interrupts_enabled(false);
				for (uint64_t sliceEnd = now + SliceNs; now < sliceEnd;) {
					uint64_t next = mach_absolute_time();
					if (next - now > GapNs) {
						gapCount++;
						gapSum += next - now;
						if (next - now > gapMax)
							gapMax = next - now;
					}
					now = next;
				}
				ml_set_interrupts_enabled(enabled);
				now = mach_absolute_time();
			}
			uint64_t nowMach = mach_absolute_time(), ns = 0, sinceStart = 0;
			absolutetime_to_nanoseconds(nowMach - lastMach, &ns);
			absolutetime_to_nanoseconds(nowMach - startMach, &sinceStart);
			uint64_t nowHpet = hpet ? hpet[0xF0 / 8] : 0;
			uint32_t nowPm = inl(PmTimer);
			// HPET: 32- or 64-bit main counter; the difference in a 250 ms window fits either way.
			uint64_t hpetMs = hpet && hpetPeriodFs ? ((nowHpet - lastHpet) & 0xFFFFFFFFULL) * hpetPeriodFs / 1000000000000ULL : 0;
			// ACPI PM timer: 3.579545 MHz; 24-bit on the HP 15-f008cl (Linux: acpi_pm mask 0xffffff), so use 24 bits.
			uint64_t pmMs = static_cast<uint64_t>((nowPm - lastPm) & 0xFFFFFF) * 1000 / 3579545;
			lastMach = nowMach;
			lastHpet = nowHpet;
			lastPm = nowPm;

			char irr[48] = "?", isr[48] = "?";
			uint32_t tpr = 0, lvtt = 0, init = 0, cur = 0, apicId = 0;
			if (lapic) {
				apicId = lapic[0x20 / 4] >> 24;
				vectors(lapic, 0x200, irr, sizeof(irr));
				vectors(lapic, 0x100, isr, sizeof(isr));
				tpr = lapic[0x80 / 4];
				lvtt = lapic[0x320 / 4];
				init = lapic[0x380 / 4];
				cur = lapic[0x390 / 4];
			}
			uint16_t pm1s = inw(Pm1Status), pm1e = inw(Pm1Enable);
			uint32_t gpes = inl(Gpe0Status), gpee = inl(Gpe0Enable);

			char hp[96] = "?";
			if (hpet) {
				size_t len = static_cast<size_t>(snprintf(hp, sizeof(hp), "%llx/%llx", hpet[0x10 / 8], hpet[0x20 / 8]));
				for (uint32_t t = 0; t < 3 && len + 16 < sizeof(hp); t++) {
					uint64_t conf = hpet[(0x100 + 0x20 * t) / 8];
					if (conf & (1 << 2))  // Tn_INT_ENB_CNF
						len += static_cast<size_t>(snprintf(hp + len, sizeof(hp) - len, " t%u=%llx", t, conf & 0xFFFF));
				}
			}

			char io[160] = "-";
			if (ioapic && oneCpu) {
				// Index/data access: interrupts off so the kernel's own I/O APIC code can't run in between.
				bool enabled = ml_set_interrupts_enabled(false);
				size_t len = 0;
				for (uint32_t pin = 0; pin < 24 && len + 12 < sizeof(io); pin++) {
					ioapic[0] = 0x10 + 2 * pin;
					uint32_t low = ioapic[0x10 / 4];
					if (low & (1 << 16))  // masked
						continue;
					len += static_cast<size_t>(snprintf(io + len, sizeof(io) - len, "%s%u:%x%s%s", len ? "," : "", pin,
						low & 0xFF, (low & (1 << 15)) ? "L" : "", (low & (1 << 14)) ? "R" : ""));
				}
				ml_set_interrupts_enabled(enabled);
			}

			IOLog("CRP %u t=%llu d=%llu/%llu/%llu irr=%s isr=%s tpr=%x lvtt=%x/%x/%x pm1=%04x/%04x gpe=%08x/%08x hpet=%s "
				"ioapic=%s cpu=%u smm=%llu/%llu/%llu idle=%s\n", n, sinceStart / 1000000, ns / 1000000, hpetMs, pmMs, irr,
				isr, tpr, lvtt, init, cur, pm1s, pm1e, gpes, gpee, hp, io, apicId, gapCount, gapSum / 1000, gapMax / 1000,
				idleState);
			// crprobepanic=<s>: panic from here, the one thread known to survive the freeze. On the way into the
			// debugger XNU stops every other CPU and names those that don't answer ("mp_kdp_enter() NMI pending on
			// cpus", "Uninterruptible processor(s)"), i.e. any CPU stuck with interrupts off.
			if (panicAt && sinceStart / 1000000000ULL >= panicAt)
				panic("CRP: crprobepanic=%u: deliberate panic from the platform probe", panicAt);
		}
		OSSafeReleaseNULL(lapicMap);
		OSSafeReleaseNULL(hpetMap);
		OSSafeReleaseNULL(ioapicMap);
		IOLog("CRP: done\n");
	}
}

void PlatformProbe::start() {
	static bool started;
	if (started || !checkKernelArgument("-crplatprobe"))
		return;
	started = true;
	thread_t thread;
	if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS)
		thread_deallocate(thread);
	else
		IOLog("CRP: failed to start the probe thread\n");
}

void PlatformProbe::checkIdle(KernelPatcher &patcher) {
	auto func = patcher.solveSymbol(KernelPatcher::KernelID, "_machine_idle");
	patcher.clearError();
	if (!func) {
		idleState = "nosym";
		return;
	}
	// Monterey 21.6.0: `sti; hlt; cli` once, at +0x1C8; the --nohlt patch makes it `sti; nop; cli`.
	auto *code = reinterpret_cast<const uint8_t *>(func);
	for (size_t i = 0; i + 2 < 0x400; i++) {
		if (code[i] != 0xFB || code[i + 2] != 0xFA || (code[i + 1] != 0xF4 && code[i + 1] != 0x90))
			continue;
		idleState = code[i + 1] == 0xF4 ? "hlt" : "nop";
		IOLog("CRP: machine_idle: %s at +0x%zx\n", idleState, i);
		return;
	}
}
