#include "platform/Cores.hpp"
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
OnCore::OnCore([[maybe_unused]] int core) {
#ifdef __SWITCH__
  u64 allowed = 0;
  s32 was = 0;
  u64 wasMask = 0;
  if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !((allowed >> core) & 1) ||
      R_FAILED(svcGetThreadCoreMask(&was, &wasMask, CUR_THREAD_HANDLE)))
    return;
  if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, u32(1u << core)))) {
    moved_ = true;
    core_ = was;
    mask_ = wasMask;
  }
#endif
}
OnCore::~OnCore() {
#ifdef __SWITCH__
  if (moved_)
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core_, u32(mask_));
#endif
}
} // namespace foil::platform
