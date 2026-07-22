#ifdef __cplusplus
extern "C" {
#endif
#include <sys/termios.h>

/* c_cc indexes — Linux/glibc compatible */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#undef VTIME
#define VTIME    5
#undef VMIN
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

#ifdef __cplusplus
}
#endif
