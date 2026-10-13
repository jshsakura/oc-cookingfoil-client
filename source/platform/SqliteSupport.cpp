// Two POSIX calls SQLite's file layer names that the console's C library
// lacks. Files there have no owners, so changing one is a no-op that
// succeeds, and the app is whoever it needs to be.
#ifdef __SWITCH__
#include <sys/types.h>
extern "C" int fchown(int, uid_t, gid_t) { return 0; }
extern "C" uid_t geteuid(void) { return 0; }
#endif
