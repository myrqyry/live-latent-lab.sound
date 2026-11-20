#pragma once
/* Single-host, little-endian float32 PCM. One request -> one reply.
 * Private AF_UNIX socket: NOT a remote or authenticated network protocol. */
#include <stdint.h>
#define MVC2_MAGIC 0x3243564du
#define MVC2_FRAMES 2560u
#define MVC2_MAX_OUTPUT 8192u
struct mvc2_header { uint32_t magic; uint32_t frames; };

#if defined(__cplusplus)
static_assert(sizeof(mvc2_header)==8, "MeanVC2 protocol header layout");
#else
_Static_assert(sizeof(struct mvc2_header)==8, "MeanVC2 protocol header layout");
#endif
