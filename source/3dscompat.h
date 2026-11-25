#ifndef _3DSCOMPAT_H_
#define _3DSCOMPAT_H_

// Small integer/boolean aliases the shell inherited from the Snes9x port headers.
#include <stdint.h>

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif
