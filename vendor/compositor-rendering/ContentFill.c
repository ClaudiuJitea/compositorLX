#include "ContentFill.h"
#include "PatchPixels.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}
static double match(const uint8_t *pixels, size_t stride, const uint8_t *known,
                    int w, int h, int p, int q, int radius) {
    int px=p%w, py=p/w, qx=q%w, qy=q/w, count=0;
    double sum=0;
    for(int dy=-radius; dy<=radius; ++dy) for(int dx=-radius; dx<=radius; ++dx) {
        int x=px+dx,y=py+dy,sx=qx+dx,sy=qy+dy;
        if(x<0||y<0||x>=w||y>=h||sx<0||sy<0||sx>=w||sy>=h||!known[y*w+x]) continue;
        const uint8_t *a=pixels+y*stride+x*4, *b=pixels+sy*stride+sx*4;
        for(int c=0;c<4;++c) { int d=a[c]-b[c]; sum+=d*d; }
        ++count;
    }
    return count ? sum/count : DBL_MAX;
}
int content_fill(uint8_t *pixels, size_t stride, const uint8_t *mask, size_t ms, int w, int h) {
    return patch_fill(pixels, stride, mask, ms, w, h, 0);
}
