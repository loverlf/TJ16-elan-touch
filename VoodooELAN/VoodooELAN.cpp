//
//  VoodooELAN.cpp
//  VoodooELAN
//
//  USB ELAN touchscreen -> VoodooInput client.
//
//  ELAN report layout (from the TJ16-touchscreen userspace reference):
//    - report[0]            == 0x01 (report ID, touch)
//    - last byte            == reported contact count
//    - primary point (slot 0): x = report[4]|report[5]<<8,
//                              y = report[8]|report[9]<<8
//    - extra points: offset = 12 + (slot-1)*11,
//                    x = report[offset+3]|report[offset+4]<<8,
//                    y = report[offset+7]|report[offset+8]<<8
//    - a point is valid only if 0 < x <= 3168 && 0 < y <= 1968
//
#include "VoodooELAN.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/usb/IOUSBHostIOSource.h>
#include <IOKit/usb/StandardUSB.h>
#include <mach/mach_time.h>
#include <kern/task.h>
#include <string.h>

#include "VoodooInputMultitouch/VoodooInputMessages.h"

#define super IOService
OSDefineMetaClassAndStructors(VoodooELAN, IOService);

// ELAN raw report geometry.
static constexpr uint16_t kRawXMax   = 3168;
static constexpr uint16_t kRawYMax   = 1968;
static constexpr uint8_t  kMaxPoints = 10;
static constexpr uint8_t  kReportIdTouch = 0x01;
static constexpr uint8_t  kMinReportLength = 12;   // 2 (header) + 10 (slot)

bool VoodooELAN::start(IOService* provider) {
    if (!super::start(provider)) {
        IOLog("VoodooELAN: super::start failed\n");
        return false;
    }

    interface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!interface) {
        IOLog("VoodooELAN: provider is not an IOUSBHostInterface\n");
        return false;
    }

    // Advertise properties VoodooInput reads from the IORegistry plane.
    // VoodooInput attaches to any IOService with VoodooInputSupported=true.
    setProperty("VoodooInputSupported", kOSBooleanTrue);
    setProperty("IOFBTransform", 0ull, 8);          // kIOFBRotate0
    setProperty("Logical Max X", kRawXMax, 32);
    setProperty("Logical Max Y", kRawYMax, 32);
    setProperty("Physical Max X", kRawXMax, 32);
    setProperty("Physical Max Y", kRawYMax, 32);

    if (!interface->open(this)) {
        IOLog("VoodooELAN: could not open USB interface\n");
        return false;
    }

    if (!setupPipe()) {
        IOLog("VoodooELAN: could not set up interrupt pipe\n");
        return false;
    }

    startRead();
    IOLog("VoodooELAN: started on ELAN 04F3:2F05, listening for touch\n");
    return true;
}

bool VoodooELAN::setupPipe() {
    const StandardUSB::ConfigurationDescriptor* configDesc = interface->getConfigurationDescriptor();
    const StandardUSB::InterfaceDescriptor* ifaceDesc = interface->getInterfaceDescriptor();
    if (!configDesc || !ifaceDesc) {
        IOLog("VoodooELAN: missing USB descriptors\n");
        return false;
    }

    // Find the interrupt IN endpoint.
    const StandardUSB::EndpointDescriptor* ep = nullptr;
    const IOUSBDescriptorHeader* current = nullptr;
    while ((current = StandardUSB::getNextAssociatedDescriptorWithType(
                configDesc, ifaceDesc, current, StandardUSB::kDescriptorTypeEndpoint)) != nullptr) {
        const StandardUSB::EndpointDescriptor* e = (const StandardUSB::EndpointDescriptor*)current;
        if (StandardUSB::getEndpointType(e) != StandardUSB::kUSBAttributeInterrupt) continue;
        if (StandardUSB::getEndpointDirection(e) != StandardUSB::kUSBEndpointDirectionIn) continue;
        readBufferSize = StandardUSB::getEndpointMaxPacketSize(e);
        interruptPipe = interface->copyPipe(StandardUSB::getEndpointAddress(e));
        break;
    }

    if (!interruptPipe || readBufferSize == 0) {
        IOLog("VoodooELAN: no interrupt IN endpoint found\n");
        return false;
    }

    readBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn, readBufferSize);
    if (!readBuffer) {
        IOLog("VoodooELAN: could not allocate read buffer\n");
        return false;
    }

    readCompletion = IOUSBHostCompletion::withAction(&readCompletionAction, this);
    if (!readCompletion) {
        IOLog("VoodooELAN: could not allocate completion\n");
        return false;
    }

    return true;
}

void VoodooELAN::startRead() {
    if (!interruptPipe || !readBuffer || !readCompletion) return;
    IOReturn status = interruptPipe->io(readBuffer, readBufferSize, readCompletion, 0);
    if (status != kIOReturnSuccess) {
        IOLog("VoodooELAN: failed to queue read (0x%x)\n", status);
    }
}

void VoodooELAN::readCompletionAction(void* target, void* parameter, IOReturn status, uint32_t actualByteCount) {
    VoodooELAN* me = static_cast<VoodooELAN*>(target);
    me->handleReport(status, actualByteCount);
}

void VoodooELAN::handleReport(IOReturn status, uint32_t actualByteCount) {
    if (status != kIOReturnSuccess || !readBuffer) {
        // Device error/removal; do not resubscribe.
        return;
    }
    if (actualByteCount > 0) {
        const uint8_t* report = static_cast<const uint8_t*>(readBuffer->getBytesNoCopy());
        sendEvent(report, actualByteCount);
    }
    startRead();
}

void VoodooELAN::sendEvent(const uint8_t* report, uint32_t reportLength) {
    if (reportLength < kMinReportLength || report[0] != kReportIdTouch) {
        return;
    }

    int reportedCount = report[reportLength - 1];
    if (reportedCount < 0) reportedCount = 0;
    if (reportedCount > kMaxPoints) reportedCount = kMaxPoints;

    uint16_t xs[kMaxPoints];
    uint16_t ys[kMaxPoints];
    int count = 0;

    // Primary point (slot 0).
    if (reportedCount >= 1) {
        uint16_t x = (uint16_t)(report[4] | (report[5] << 8));
        uint16_t y = (uint16_t)(report[8] | (report[9] << 8));
        if (x > 0 && y > 0 && x <= kRawXMax && y <= kRawYMax) {
            xs[count] = x;
            ys[count] = y;
            count++;
        }
    }

    // Additional points.
    for (int slot = 1; slot < reportedCount && count < kMaxPoints; slot++) {
        int offset = 12 + (slot - 1) * 11;
        if (offset + 9 >= (int)reportLength) break;
        uint16_t x = (uint16_t)(report[offset + 3] | (report[offset + 4] << 8));
        uint16_t y = (uint16_t)(report[offset + 7] | (report[offset + 8] << 8));
        if (x > 0 && y > 0 && x <= kRawXMax && y <= kRawYMax) {
            xs[count] = x;
            ys[count] = y;
            count++;
        }
    }

    VoodooInputEvent event;
    memset(&event, 0, sizeof(event));
    event.timestamp = mach_absolute_time();
    event.contact_count = static_cast<uint8_t>(count);

    static const MT2FingerType fingerTypes[5] = {
        kMT2FingerTypeThumb,
        kMT2FingerTypeIndexFinger,
        kMT2FingerTypeMiddleFinger,
        kMT2FingerTypeRingFinger,
        kMT2FingerTypeLittleFinger,
    };

    for (int i = 0; i < count; i++) {
        VoodooInputTransducer& t = event.transducers[i];
        t.timestamp = event.timestamp;
        t.secondaryId = static_cast<UInt32>(i);   // slot-based tracking id
        t.type = FINGER;
        t.fingerType = fingerTypes[i % 5];
        t.isValid = true;
        t.isPhysicalButtonDown = false;
        t.isTransducerActive = true;
        t.supportsPressure = false;

        t.currentCoordinates.x = xs[i];
        t.currentCoordinates.y = ys[i];
        t.currentCoordinates.pressure = 1;
        t.currentCoordinates.width = 4;

        t.previousCoordinates.x = prevX[i];
        t.previousCoordinates.y = prevY[i];
        t.previousCoordinates.pressure = 1;
        t.previousCoordinates.width = 4;

        t.maxPressure = 255;

        prevX[i] = xs[i];
        prevY[i] = ys[i];
    }

    for (int i = count; i < kMaxPoints; i++) {
        event.transducers[i].isValid = false;
        event.transducers[i].isTransducerActive = false;
        prevX[i] = 0;
        prevY[i] = 0;
    }

    if (!voodooInput) {
        voodooInput = findVoodooInput();
    }
    if (voodooInput) {
        voodooInput->message(kIOMessageVoodooInputMessage, this, &event);
    }
}

IOService* VoodooELAN::findVoodooInput() {
    OSArray* children = copyChildServices(gIOServicePlane);
    if (!children) return nullptr;

    IOService* found = nullptr;
    for (unsigned int i = 0; i < children->getCount(); i++) {
        IOService* child = OSDynamicCast(IOService, children->getObject(i));
        if (child && child->getProperty(VOODOO_INPUT_IDENTIFIER) != nullptr) {
            found = child;
            break;
        }
    }
    children->release();
    return found;
}

bool VoodooELAN::willTerminate(IOService* provider, IOOptionBits options) {
    if (interruptPipe) {
        interruptPipe->abort();
    }
    return super::willTerminate(provider, options);
}

void VoodooELAN::stop(IOService* provider) {
    if (interruptPipe) {
        interruptPipe->abort();
        interruptPipe->release();
        interruptPipe = nullptr;
    }
    if (interface) {
        interface->close(this);
        interface = nullptr;
    }
    if (readBuffer) {
        readBuffer->release();
        readBuffer = nullptr;
    }
    if (readCompletion) {
        readCompletion->release();
        readCompletion = nullptr;
    }
    voodooInput = nullptr;
    super::stop(provider);
}

void VoodooELAN::free() {
    super::free();
}
