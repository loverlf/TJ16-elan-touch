//
//  VoodooELAN.hpp
//  VoodooELAN
//
//  A USB ELAN multitouch screen -> VoodooInput client driver.
//  Matches 04F3:2F05 (ELAN Touchscreen, 10-point), reads HID interrupt
//  reports in the kernel, parses ELAN raw points, and feeds a
//  VoodooInputEvent to the attached VoodooInput provider, which
//  masquerades as a Magic Trackpad 2 for native macOS gestures.
//
#ifndef VOODOO_ELAN_HPP
#define VOODOO_ELAN_HPP

#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

class VoodooELAN : public IOService {
    OSDeclareDefaultStructors(VoodooELAN);

private:
    IOUSBHostInterface* interface = nullptr;
    IOUSBHostPipe* interruptPipe = nullptr;
    IOBufferMemoryDescriptor* readBuffer = nullptr;
    UInt32 readBufferSize = 0;
    IOUSBHostCompletion readCompletion;   // plain struct, copied by io()
    IOService* voodooInput = nullptr;

    // Per-slot previous coordinates so VoodooInput / AppleMultitouch can
    // derive velocities (needed for scrolling and swipe gestures).
    uint16_t prevX[10];
    uint16_t prevY[10];

    bool setupPipe();
    void startRead();
    void handleReport(IOReturn status, uint32_t actualByteCount);
    void sendEvent(const uint8_t* report, uint32_t reportLength);
    IOService* findVoodooInput();
    static void readCompletionAction(void* target, void* parameter, IOReturn status, uint32_t actualByteCount);

public:
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    bool willTerminate(IOService* provider, IOOptionBits options) override;
    void free() override;
};

#endif /* VOODOO_ELAN_HPP */
