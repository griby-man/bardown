// camera exports the sdk leaves out of its runtime build (xboxkrnl_usbcam.cpp is commented out).
// nhl 14 only probes for the vision camera for gameface photos, so report that none is plugged in.

#include <rex/hook.h>
#include <rex/types.h>

namespace bardown {

rex::u32 XUsbcamGetState_entry() { return 0; }  // 0 = not connected

// takes an XUSBCAM_CONFIG*, irrelevant with no device
rex::u32 XUsbcamSetConfig_entry() { return 0x48F; }  // ERROR_DEVICE_NOT_CONNECTED

}  // namespace bardown

REX_EXPORT(__imp__XUsbcamGetState, bardown::XUsbcamGetState_entry)
REX_EXPORT(__imp__XUsbcamSetConfig, bardown::XUsbcamSetConfig_entry)
