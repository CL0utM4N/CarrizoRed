//
//  FCHXHCIFix.cpp
//
//  Makes USB usable on AMD FCH xHCI controllers whose interrupts never reach macOS.
//
//  On the ASUS VivoBook 15 D509BA (A9-9425 Stoney Ridge), the FCH xHCI (1022:7914) completes transfers but none of its
//  interrupts arrive, with plain MSI and with MSI-X alike, although the controller sends them (IMAN.IP keeps clearing,
//  as MSI does on send) and the GPU's MSIs to the same address do arrive. AppleUSBXHCI then only services the
//  controller from its 2 s fallback timer, so every transfer waits up to 2 s: USB storage crawls at ~130 KB/s and
//  input devices lag. Signalling only when USBSTS.EINT was set did not help (the controller raises EINT just as
//  rarely); signalling continuously does.
//
//  This driver attaches to the controller in its own IOMatchCategory, next to AppleUSBXHCIPCI. A thread waits for
//  AppleUSBXHCIPCI, finds its interrupt source (the IOFilterInterruptEventSource on the driver's work loop whose
//  provider is the controller), and signals it every poll (~2.5 ms), which runs AppleUSBXHCI's interrupt handler on its
//  work loop exactly as a delivered interrupt would; the handler returns quickly when the event rings are empty.
//  USBSTS.EINT is cleared (RW1C) when seen. The state is published in the controller's "FCHXHCIFix-state" property.
//
//  Boot-args: -fchxhcioff      do nothing
//             fchxhcimode=1    signal only when USBSTS.EINT is set (diagnosis; doesn't fix the D509BA)
//
//  It needs no hooks: everything goes through IOKit's public interfaces, except the work loop's event chain, which is
//  read through the IOWorkLoop / IOEventSource layout from the SDK headers.
//

#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <string.h>

class FCHXHCIFix : public IOService {
	OSDeclareDefaultStructors(FCHXHCIFix)

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

private:
	static void forwardThread(void *arg, wait_result_t);
	static IOReturn collectSources(OSObject *owner, void *loop, void *, void *, void *);
	void publish(const char *what);

	IOPCIDevice *pci {nullptr};
	IOFilterInterruptEventSource *sources[4] {};
	uint32_t sourceCount {0}, forwards {0}, polls {0}, mode {2};
	volatile bool stopping {false};
};

OSDefineMetaClassAndStructors(FCHXHCIFix, IOService)

namespace {
	// The event chain is protected; these read it through the kernel's own layout.
	struct WorkLoopPeek : IOWorkLoop {
		static IOEventSource *head(IOWorkLoop *loop) { return static_cast<WorkLoopPeek *>(loop)->eventChain; }
	};
	struct EventSourcePeek : IOEventSource {
		static IOEventSource *next(IOEventSource *source) { return static_cast<EventSourcePeek *>(source)->eventChainNext; }
	};
}

bool FCHXHCIFix::start(IOService *provider) {
	char arg[8];
	if (PE_parse_boot_argn("-fchxhcioff", arg, sizeof(arg)))
		return false;
	pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci || !super::start(provider))
		return false;
	if (!PE_parse_boot_argn("fchxhcimode", &mode, sizeof(mode)) || !mode)
		mode = 2;
	pci->retain();
	retain();  // held by the thread
	thread_t thread;
	if (kernel_thread_start(forwardThread, this, &thread) != KERN_SUCCESS) {
		release();
		publish("thread FAILED");
		return true;
	}
	thread_deallocate(thread);
	publish("waiting for AppleUSBXHCIPCI");
	return true;
}

void FCHXHCIFix::stop(IOService *provider) {
	stopping = true;
	super::stop(provider);
}

void FCHXHCIFix::publish(const char *what) {
	char state[160];
	snprintf(state, sizeof(state), "%u sources, %u forwards, %u polls; %s", sourceCount,
		__atomic_load_n(&forwards, __ATOMIC_RELAXED), __atomic_load_n(&polls, __ATOMIC_RELAXED), what);
	pci->setProperty("FCHXHCIFix-state", state);
}

// Collect the driver's filter interrupt sources on the controller, inside the work loop's gate.
IOReturn FCHXHCIFix::collectSources(OSObject *owner, void *arg, void *, void *, void *) {
	auto *self = static_cast<FCHXHCIFix *>(owner);
	auto *loop = static_cast<IOWorkLoop *>(arg);
	for (auto *source = WorkLoopPeek::head(loop); source && self->sourceCount < sizeof(self->sources) / sizeof(self->sources[0]);
		 source = EventSourcePeek::next(source)) {
		auto *filter = OSDynamicCast(IOFilterInterruptEventSource, source);
		if (filter && filter->getProvider() == self->pci) {
			filter->retain();
			self->sources[self->sourceCount++] = filter;
		}
	}
	return kIOReturnSuccess;
}

void FCHXHCIFix::forwardThread(void *arg, wait_result_t) {
	auto *self = static_cast<FCHXHCIFix *>(arg);
	IOService *driver = nullptr;
	IOMemoryMap *map = nullptr;

	// AppleUSBXHCIPCI attaches to the same controller in the default category; give it up to 5 min.
	for (int i = 0; i < 3000 && !driver && !self->stopping; i++) {
		if (auto *clients = self->pci->getClientIterator()) {
			while (auto *client = OSDynamicCast(IOService, clients->getNextObject())) {
				if (!strcmp(client->getMetaClass()->getClassName(), "AppleUSBXHCIPCI")) {
					driver = client;
					driver->retain();
					break;
				}
			}
			clients->release();
		}
		if (!driver)
			IOSleep(100);
	}
	if (!driver) {
		self->publish("AppleUSBXHCIPCI never attached");
		goto done;
	}
	if (auto *loop = driver->getWorkLoop()) {
		// The source is added during start(); retry briefly if it isn't there yet.
		for (int i = 0; i < 50 && !self->sourceCount && !self->stopping; i++) {
			loop->runAction(collectSources, self, loop);
			if (!self->sourceCount)
				IOSleep(100);
		}
	}
	if (!self->sourceCount) {
		self->publish("no interrupt source on AppleUSBXHCIPCI's work loop");
		goto done;
	}
	map = self->pci->mapDeviceMemoryWithIndex(0);
	if (!map) {
		self->publish("BAR0 map FAILED");
		goto done;
	}
	IOLog("FCHXHCIFix: forwarding interrupts to AppleUSBXHCIPCI (%u sources, mode %u)\n", self->sourceCount, self->mode);
	{
		auto *base = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
		uint64_t length = map->getLength();
		auto r32 = [&](uint32_t off) { return off + 4 <= length ? *reinterpret_cast<volatile uint32_t *>(base + off) : ~0u; };
		while (!self->stopping) {
			uint32_t cap = r32(0);
			if (cap == ~0u) {  // powered down (D3) or gone
				IOSleep(10);
				continue;
			}
			uint32_t usbsts = (cap & 0xFF) + 4;
			uint32_t sts = r32(usbsts);
			uint32_t polls = __atomic_add_fetch(&self->polls, 1, __ATOMIC_RELAXED);
			bool eint = sts != ~0u && (sts & 8);
			if (eint)
				*reinterpret_cast<volatile uint32_t *>(base + usbsts) = 8;
			if (eint || self->mode >= 2) {
				for (uint32_t i = 0; i < self->sourceCount; i++)
					self->sources[i]->signalInterrupt();
				__atomic_fetch_add(&self->forwards, 1, __ATOMIC_RELAXED);
			}
			if (polls % 1000 == 1)
				self->publish(self->mode >= 2 ? "running, mode 2 (every poll)" : "running, mode 1 (on EINT)");
			IOSleep(1);
		}
	}

done:
	if (map)
		map->release();
	for (uint32_t i = 0; i < self->sourceCount; i++)
		self->sources[i]->release();
	self->sourceCount = 0;
	if (driver)
		driver->release();
	self->pci->release();
	self->release();
}
