// What mbedTLS asks the platform for (scripts/build-tls.sh): randomness and a
// millisecond clock, both from the console.
#ifdef __SWITCH__
#include <switch.h>
#include <cstddef>
#include <cstdint>
extern "C" {
int mbedtls_hardware_poll(void *, unsigned char *output, size_t len, size_t *olen) {
  // The csrng service (main.cpp), or libnx's generator seeded from the
  // kernel's entropy when it did not open.
  if (R_FAILED(csrngGetRandomBytes(output, len))) randomGet(output, len);
  if (olen) *olen = len;
  return 0;
}
int64_t mbedtls_ms_time(void) { return int64_t(armTicksToNs(armGetSystemTick()) / 1000000); }
}
#endif
