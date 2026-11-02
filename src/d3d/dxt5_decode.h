#ifndef XBR_DXT5_DECODE_H
#define XBR_DXT5_DECODE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static void xbr_decode_dxt5_rgba(const uint8_t *src, unsigned pitch,
                                unsigned width, unsigned height, uint8_t *dst)
{
    for (unsigned by = 0; by < (height + 3) / 4; ++by) {
        for (unsigned bx = 0; bx < (width + 3) / 4; ++bx) {
            const uint8_t *b = src + by * pitch + bx * 16;
            unsigned alpha[8] = {b[0], b[1]};
            uint64_t ai = 0;
            uint32_t ci = 0;
            unsigned rgb[4][3];
            for (unsigned i = 0; i < 6; ++i) ai |= (uint64_t)b[2+i] << (8*i);
            for (unsigned i = 0; i < 4; ++i) ci |= (uint32_t)b[12+i] << (8*i);
            if (alpha[0] > alpha[1]) {
                for (unsigned i = 1; i <= 6; ++i)
                    alpha[i+1] = ((7-i)*alpha[0] + i*alpha[1]) / 7;
            } else {
                for (unsigned i = 1; i <= 4; ++i)
                    alpha[i+1] = ((5-i)*alpha[0] + i*alpha[1]) / 5;
                alpha[6] = 0; alpha[7] = 255;
            }
            for (unsigned i = 0; i < 2; ++i) {
                unsigned c = b[8+i*2] | ((unsigned)b[9+i*2] << 8);
                unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
                rgb[i][0] = (r << 3) | (r >> 2);
                rgb[i][1] = (g << 2) | (g >> 4);
                rgb[i][2] = (bl << 3) | (bl >> 2);
            }
            for (unsigned c = 0; c < 3; ++c) {
                rgb[2][c] = (2*rgb[0][c] + rgb[1][c]) / 3;
                rgb[3][c] = (rgb[0][c] + 2*rgb[1][c]) / 3;
            }
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
                unsigned p = y*4+x, dx = bx*4+x, dy = by*4+y;
                if (dx >= width || dy >= height) continue;
                uint8_t *out = dst + ((size_t)dy*width+dx)*4;
                unsigned color = (ci >> (p*2)) & 3;
                for (unsigned c = 0; c < 3; ++c) out[c] = (uint8_t)rgb[color][c];
                out[3] = (uint8_t)alpha[(ai >> (p*3)) & 7];
            }
        }
    }
}
static void xbr_decode_dxt1_rgba(const uint8_t *src, unsigned pitch,
                                unsigned width, unsigned height, uint8_t *dst)
{
    for (unsigned by=0;by<(height+3)/4;++by) for(unsigned bx=0;bx<(width+3)/4;++bx) {
        const uint8_t *b=src+(size_t)by*pitch+bx*8;
        unsigned colors[4][4]={{0}}, endpoints[2];
        uint32_t indices=0;
        for(unsigned i=0;i<4;++i) indices|=(uint32_t)b[4+i]<<(8*i);
        for(unsigned i=0;i<2;++i) {
            unsigned c=b[i*2]|((unsigned)b[i*2+1]<<8);
            endpoints[i]=c;
            unsigned r=(c>>11)&31,g=(c>>5)&63,bl=c&31;
            colors[i][0]=(r<<3)|(r>>2);colors[i][1]=(g<<2)|(g>>4);
            colors[i][2]=(bl<<3)|(bl>>2);colors[i][3]=255;
        }
        for(unsigned c=0;c<3;++c) {
            if(endpoints[0]>endpoints[1]) {
                colors[2][c]=(2*colors[0][c]+colors[1][c])/3;
                colors[3][c]=(colors[0][c]+2*colors[1][c])/3;
            } else colors[2][c]=(colors[0][c]+colors[1][c])/2;
        }
        colors[2][3]=255;colors[3][3]=endpoints[0]>endpoints[1]?255:0;
        for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
            unsigned dx=bx*4+x,dy=by*4+y;
            if(dx>=width||dy>=height) continue;
            unsigned index=(indices>>(2*(y*4+x)))&3;
            for(unsigned c=0;c<4;++c) dst[((size_t)dy*width+dx)*4+c]=(uint8_t)colors[index][c];
        }
    }
}
/* Xbox compressed volumes store groups of up to four depth slices per
 * 4x4 XY block, rather than a whole 2D image per depth slice. */
static void xbr_decode_dxt_volume_slice(const uint8_t *src, unsigned width,
    unsigned height, unsigned depth, unsigned z, int bc1, uint8_t *dst)
{
    unsigned nx=(width+3)/4,ny=(height+3)/4;
    unsigned group=z&~3u,group_depth=depth-group;
    if(group_depth>4)group_depth=4;
    unsigned block_bytes=bc1?8:16;
    for(unsigned by=0;by<ny;++by)for(unsigned bx=0;bx<nx;++bx){
        size_t block=(size_t)group*nx*ny+((size_t)by*nx+bx)*group_depth+(z-group);
        uint8_t pixels[64];
        if(bc1)xbr_decode_dxt1_rgba(src+block*block_bytes,block_bytes,4,4,pixels);
        else xbr_decode_dxt5_rgba(src+block*block_bytes,block_bytes,4,4,pixels);
        unsigned count=width-bx*4;if(count>4)count=4;
        for(unsigned y=0;y<4 && by*4+y<height;++y)
            memcpy(dst+((size_t)(by*4+y)*width+bx*4)*4,pixels+y*16,count*4);
    }
}
#endif
