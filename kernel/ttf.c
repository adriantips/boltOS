/* ===========================================================================
 *  BoltOS  -  kernel/ttf.c
 *  A small TrueType rasterizer: parses an embedded .ttf and renders glyph
 *  outlines to an anti-aliased 8-bit coverage bitmap. Quadratic-bezier outlines
 *  are flattened to line segments and filled with a non-zero winding scanline
 *  fill at 4x supersampling, then box-downsampled for anti-aliasing. All math
 *  is integer/fixed-point (the kernel avoids relying on the FPU). Simple glyphs
 *  only (Latin letters/digits are simple); composite glyphs render empty.
 * ===========================================================================*/
#include <stdint.h>
#include "ttf.h"
#include "kprintf.h"

extern const uint8_t _binary_font_ttf_start[];
extern const uint8_t _binary_font_ttf_end[];

/* --- big-endian readers -------------------------------------------------- */
static const uint8_t *F;            /* font base */
static uint32_t Flen;
static uint16_t be16(uint32_t o){ return (uint16_t)((F[o]<<8)|F[o+1]); }
static int16_t  i16 (uint32_t o){ return (int16_t)be16(o); }
static uint32_t be32(uint32_t o){ return ((uint32_t)F[o]<<24)|((uint32_t)F[o+1]<<16)|((uint32_t)F[o+2]<<8)|F[o+3]; }

static uint32_t t_head, t_maxp, t_cmap, t_loca, t_glyf, t_hhea, t_hmtx;
static uint16_t units_per_em, num_glyphs, loca_long, num_hmetrics;
static int16_t  font_ascent, font_descent;
static uint32_t cmap_sub;            /* chosen cmap subtable (format 4) */
static int      ready;

static uint32_t find_table(const char *tag) {
    uint16_t n = be16(4);
    for (uint16_t i = 0; i < n; i++) {
        uint32_t rec = 12 + i * 16;
        if (F[rec]==tag[0] && F[rec+1]==tag[1] && F[rec+2]==tag[2] && F[rec+3]==tag[3])
            return be32(rec + 8);
    }
    return 0;
}

int ttf_init(void) {
    F = _binary_font_ttf_start;
    Flen = (uint32_t)(_binary_font_ttf_end - _binary_font_ttf_start);
    if (Flen < 12) return -1;
    uint32_t ver = be32(0);
    if (ver != 0x00010000 && ver != 0x74727565) return -1;   /* 1.0 / 'true' */
    t_head = find_table("head"); t_maxp = find_table("maxp");
    t_cmap = find_table("cmap"); t_loca = find_table("loca");
    t_glyf = find_table("glyf"); t_hhea = find_table("hhea");
    t_hmtx = find_table("hmtx");
    if (!t_head || !t_maxp || !t_cmap || !t_loca || !t_glyf || !t_hhea) return -1;
    units_per_em = be16(t_head + 18);
    loca_long    = be16(t_head + 50);
    num_glyphs   = be16(t_maxp + 4);
    font_ascent  = i16(t_hhea + 4);
    font_descent = i16(t_hhea + 6);
    num_hmetrics = be16(t_hhea + 34);
    if (units_per_em == 0) return -1;

    /* pick a Unicode cmap subtable (prefer Windows BMP 3/1), require format 4 */
    uint16_t n = be16(t_cmap + 2);
    uint32_t best = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint32_t rec = t_cmap + 4 + i * 8;
        uint16_t plat = be16(rec), enc = be16(rec + 2);
        uint32_t off  = be32(rec + 4);
        uint32_t sub  = t_cmap + off;
        if (be16(sub) != 4) continue;                /* format 4 only */
        if ((plat == 3 && (enc == 1 || enc == 0)) || plat == 0) { best = sub; if (plat==3&&enc==1) break; }
        else if (!best) best = sub;
    }
    if (!best) return -1;
    cmap_sub = best;
    ready = 1;
    kprintf("[ok] TrueType font: %u upm, %u glyphs, ascent %d\n", units_per_em, num_glyphs, font_ascent);
    return 0;
}
int ttf_ready(void) { return ready; }

/* cmap format 4: codepoint -> glyph index */
static uint16_t glyph_of(uint32_t cp) {
    if (!ready || cp > 0xFFFF) return 0;
    uint32_t s = cmap_sub;
    uint16_t segx2 = be16(s + 6), segc = segx2 / 2;
    uint32_t endO   = s + 14;
    uint32_t startO = endO + segx2 + 2;
    uint32_t deltaO = startO + segx2;
    uint32_t rangeO = deltaO + segx2;
    for (uint16_t i = 0; i < segc; i++) {
        uint16_t end = be16(endO + i * 2);
        if (cp > end) continue;
        uint16_t start = be16(startO + i * 2);
        if (cp < start) return 0;
        int16_t  delta = i16(deltaO + i * 2);
        uint16_t ro    = be16(rangeO + i * 2);
        if (ro == 0) return (uint16_t)(cp + delta);
        uint32_t gi_addr = rangeO + i * 2 + ro + (cp - start) * 2;
        if (gi_addr + 1 >= Flen) return 0;
        uint16_t g = be16(gi_addr);
        return g ? (uint16_t)(g + delta) : 0;
    }
    return 0;
}

static uint32_t loca_off(uint16_t g) {
    return loca_long ? be32(t_loca + g * 4) : (uint32_t)be16(t_loca + g * 2) * 2;
}
static int advance_funits(uint16_t g) {
    uint16_t idx = g < num_hmetrics ? g : (num_hmetrics ? num_hmetrics - 1 : 0);
    return be16(t_hmtx + idx * 4);
}

/* --- outline -> edges ---------------------------------------------------- */
#define SS        4                  /* supersample factor per axis */
#define MAXPTS    1024
#define MAXEDGE   4096
#define MAXSUP    320                /* max supersampled dimension */
#define MAXOUT    80                 /* max output glyph dimension  */

static int   px_x[MAXPTS], px_y[MAXPTS]; static uint8_t on_c[MAXPTS];
struct edge { int x0,y0,x1,y1; };
static struct edge edges[MAXEDGE]; static int nedge;
static uint8_t  cov[MAXOUT * MAXOUT];
static uint8_t  super[MAXSUP * MAXSUP / 8 + MAXSUP];   /* 1 bit per sample (row-padded) */

static void add_edge(int x0,int y0,int x1,int y1){
    if (y0==y1) return;
    if (nedge < MAXEDGE) { edges[nedge].x0=x0; edges[nedge].y0=y0; edges[nedge].x1=x1; edges[nedge].y1=y1; nedge++; }
}
/* flatten a quadratic bezier (integers in supersample space) into edges */
static void quad(int x0,int y0,int cx,int cy,int x1,int y1){
    const int K=6;
    int px=x0, py=y0;
    for (int i=1;i<=K;i++){
        int t=i, mt=K-i;
        /* B(t) = mt^2*P0 + 2*mt*t*C + t^2*P1, over K^2 */
        int nx=(mt*mt*x0 + 2*mt*t*cx + t*t*x1)/(K*K);
        int ny=(mt*mt*y0 + 2*mt*t*cy + t*t*y1)/(K*K);
        add_edge(px,py,nx,ny); px=nx; py=ny;
    }
}

/* Render glyph index g at em-size `px` into cov[] (w x h). Fills geometry out. */
static int raster(uint16_t g, int px, int *ow, int *oh, int *obx, int *oby, int *oasc) {
    if (px < 6) px = 6; if (px > MAXOUT) px = MAXOUT;
    uint32_t go = loca_off(g), gend = loca_off(g + 1);
    int asc_px = (font_ascent * px + units_per_em/2) / units_per_em;
    *oasc = asc_px;
    if (gend <= go) { *ow=0; *oh=0; *obx=0; *oby=0; return 0; }   /* empty (space) */
    uint32_t base = t_glyf + go;
    int ncont = i16(base);
    if (ncont < 0) { *ow=0; *oh=0; *obx=0; *oby=0; return 0; }    /* composite: skip */

    uint32_t p = base + 10;
    uint16_t endpts[64]; if (ncont > 64) return -1;
    for (int i=0;i<ncont;i++){ endpts[i]=be16(p); p+=2; }
    int npts = ncont ? endpts[ncont-1]+1 : 0;
    if (npts <= 0 || npts > MAXPTS) return -1;
    uint16_t ilen = be16(p); p += 2 + ilen;          /* skip instructions */

    /* flags (with repeat) */
    static uint8_t flags[MAXPTS];
    for (int i=0;i<npts;){
        uint8_t fl = F[p++]; flags[i++]=fl;
        if (fl & 8) { uint8_t rep=F[p++]; while(rep-- && i<npts) flags[i++]=fl; }
    }
    /* x coords */
    int xc[MAXPTS]; int x=0;
    for (int i=0;i<npts;i++){ uint8_t fl=flags[i];
        if (fl&2){ uint8_t d=F[p++]; x += (fl&16)? d : -(int)d; }
        else if (!(fl&16)){ x += i16(p); p+=2; }
        xc[i]=x; }
    int yc[MAXPTS]; int y=0;
    for (int i=0;i<npts;i++){ uint8_t fl=flags[i];
        if (fl&4){ uint8_t d=F[p++]; y += (fl&32)? d : -(int)d; }
        else if (!(fl&32)){ y += i16(p); p+=2; }
        yc[i]=y; }

    /* glyph bbox in funits */
    int xmin=i16(base+2), ymin=i16(base+4), xmax=i16(base+6), ymax=i16(base+8);
    int wpx = ((xmax - xmin) * px + units_per_em - 1) / units_per_em + 2;
    int hpx = ((ymax - ymin) * px + units_per_em - 1) / units_per_em + 2;
    if (wpx < 1) wpx = 1; if (hpx < 1) hpx = 1;
    if (wpx > MAXOUT) wpx = MAXOUT; if (hpx > MAXOUT) hpx = MAXOUT;
    int Wsup = wpx*SS, Hsup = hpx*SS;
    if (Wsup > MAXSUP) Wsup = MAXSUP; if (Hsup > MAXSUP) Hsup = MAXSUP;

    *obx = (xmin * px) / units_per_em;               /* left bearing (px) */
    *oby = asc_px - (ymax * px + units_per_em - 1) / units_per_em;  /* top (px from baseline-top) */

    /* convert points to supersample space: funit -> sub-pixel, origin at bbox
     * top-left, y flipped (font y up -> raster y down) */
    nedge = 0;
    for (int i=0;i<npts;i++){
        px_x[i] = ((xc[i]-xmin) * px * SS) / units_per_em;
        px_y[i] = ((ymax-yc[i]) * px * SS) / units_per_em;
        on_c[i] = flags[i] & 1;
    }
    /* walk each contour building edges (insert implied on-curve midpoints) */
    int start=0;
    for (int c=0;c<ncont;c++){
        int last=endpts[c];
        int n=last-start+1; if (n<=0){ start=last+1; continue; }
        /* find a starting on-curve point (or synthesize) */
        int sx,sy; int si=-1;
        for (int k=0;k<n;k++){ int idx=start+k; if(on_c[idx]){ si=idx; break; } }
        if (si<0){ /* all off-curve: start at midpoint of first two */
            sx=(px_x[start]+px_x[start+ (n>1?1:0)])/2; sy=(px_y[start]+px_y[start+(n>1?1:0)])/2;
        } else { sx=px_x[si]; sy=px_y[si]; }
        int curx=sx, cury=sy;
        int havectrl=0, cxv=0, cyv=0;
        for (int k=1;k<=n;k++){
            int idx = start + ((si<0?0:si-start)+k)%n;
            int X=px_x[idx], Y=px_y[idx], onc=on_c[idx];
            if (onc){
                if (havectrl){ quad(curx,cury,cxv,cyv,X,Y); havectrl=0; }
                else add_edge(curx,cury,X,Y);
                curx=X; cury=Y;
            } else {
                if (havectrl){ int mx=(cxv+X)/2, my=(cyv+Y)/2; quad(curx,cury,cxv,cyv,mx,my); curx=mx; cury=my; }
                cxv=X; cyv=Y; havectrl=1;
            }
        }
        if (havectrl) quad(curx,cury,cxv,cyv,sx,sy);
        else add_edge(curx,cury,sx,sy);
        start=last+1;
    }

    /* scanline fill into the 1-bit supersample buffer (nonzero winding) */
    int rowbytes=(Wsup+7)/8;
    for (int i=0;i<rowbytes*Hsup;i++) super[i]=0;
    static int xs[256]; static int dir[256];
    for (int sy=0; sy<Hsup; sy++){
        int nx=0;
        for (int e=0;e<nedge && nx<256;e++){
            int y0=edges[e].y0,y1=edges[e].y1;
            int lo=y0<y1?y0:y1, hi=y0<y1?y1:y0;
            if (sy<lo || sy>=hi) continue;
            int xi = edges[e].x0 + (long)(edges[e].x1-edges[e].x0)*(sy-y0)/(y1-y0);
            xs[nx]=xi; dir[nx]=(y1>y0)?1:-1; nx++;
        }
        /* insertion sort by x */
        for (int a=1;a<nx;a++){ int vx=xs[a],vd=dir[a],b=a-1; while(b>=0&&xs[b]>vx){xs[b+1]=xs[b];dir[b+1]=dir[b];b--;} xs[b+1]=vx;dir[b+1]=vd; }
        int wind=0;
        for (int a=0;a<nx-1;a++){
            wind+=dir[a];
            if (wind!=0){
                int xa=xs[a], xb=xs[a+1]; if(xa<0)xa=0; if(xb>Wsup)xb=Wsup;
                for (int xx=xa; xx<xb; xx++) super[sy*rowbytes + (xx>>3)] |= (uint8_t)(1<<(xx&7));
            }
        }
    }
    /* box-downsample SS x SS -> 0..255 coverage */
    for (int oy=0; oy<hpx; oy++)
        for (int ox=0; ox<wpx; ox++){
            int acc=0;
            for (int j=0;j<SS;j++){ int sy=oy*SS+j; if(sy>=Hsup)break;
                for (int i=0;i<SS;i++){ int sx=ox*SS+i; if(sx>=Wsup)break;
                    if (super[sy*rowbytes+(sx>>3)] & (1<<(sx&7))) acc++; } }
            cov[oy*wpx+ox] = (uint8_t)(acc * 255 / (SS*SS));
        }
    *ow=wpx; *oh=hpx;
    return 0;
}

int ttf_glyph(uint32_t cp, int px, const uint8_t **bmp, int *w, int *h,
              int *advance, int *bx, int *by, int *asc) {
    if (!ready) return -1;
    uint16_t g = glyph_of(cp);
    int ow,oh,obx,oby,oasc;
    if (raster(g, px, &ow,&oh,&obx,&oby,&oasc) != 0) return -1;
    *bmp=cov; *w=ow; *h=oh; *bx=obx; *by=oby; *asc=oasc;
    *advance = (advance_funits(g) * px + units_per_em/2) / units_per_em;
    return 0;
}

int ttf_line_height(int px){ return ((font_ascent - font_descent) * px + units_per_em-1)/units_per_em; }

int ttf_text_width(const char *s, int px){
    if (!ready) return 0;
    int w=0;
    for (const unsigned char *p=(const unsigned char*)s; *p; p++)
        w += (advance_funits(glyph_of(*p)) * px + units_per_em/2) / units_per_em;
    return w;
}

int ttf_draw(const char *s, int x, int y_baseline, int px, ttf_plot_fn plot, void *ctx){
    if (!ready) return 0;
    int penx=x;
    for (const unsigned char *p=(const unsigned char*)s; *p; p++){
        const uint8_t *bm; int w,h,adv,bx,by,asc;
        if (ttf_glyph(*p, px, &bm,&w,&h,&adv,&bx,&by,&asc)==0){
            int gx0 = penx + bx;
            int gy0 = y_baseline - asc + by;        /* baseline - ascent gives glyph-top */
            for (int yy=0; yy<h; yy++)
                for (int xx=0; xx<w; xx++){
                    uint8_t c=bm[yy*w+xx];
                    if (c) plot(gx0+xx, gy0+yy, c, ctx);
                }
            penx += adv;
        } else penx += px/2;
    }
    return penx - x;
}
