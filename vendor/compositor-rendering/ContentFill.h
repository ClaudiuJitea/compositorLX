#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Returns 1 on success, 0 when no source patch exists, -1 on allocation failure.
int content_fill(uint8_t *rgba, size_t stride, const uint8_t *mask, size_t maskStride, int width, int height);
#ifdef __cplusplus
}
#endif
