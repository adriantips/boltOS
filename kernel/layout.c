/* ===========================================================================
 *  BoltLayout -- CSS cascade + box-model block/inline layout (see layout.h).
 * ===========================================================================*/
#include <stdint.h>
#include "layout.h"
#include "dom.h"
#include "string.h"
#include "kheap.h"

/* ----------------------------- arena ----------------------------------- */
#define LBLK (64 * 1024)
#define LMAX (8 * 1024 * 1024)
typedef struct LBk { struct LBk *next; uint32_t used; uint8_t mem[LBLK]; } LBk;
typedef struct { LBk *head; uint32_t total; int oom; } LAr;
static void *la_alloc(LAr *a, uint32_t n) {
    n = (n + 7) & ~7u; if (n > LBLK) { a->oom = 1; return 0; }
    if (!a->head || a->head->used + n > LBLK) {
        if (a->total + LBLK > LMAX) { a->oom = 1; return 0; }
        LBk *b = (LBk *)kmalloc(sizeof(LBk)); if (!b) { a->oom = 1; return 0; }
        b->next = a->head; b->used = 0; a->head = b; a->total += LBLK;
    }
    void *p = a->head->mem + a->head->used; a->head->used += n; return p;
}
static void la_free(LAr *a) { for (LBk *b = a->head; b; ) { LBk *n = b->next; kfree(b); b = n; } a->head = 0; }
typedef struct { layout_tree t; LAr ar; } LImpl;

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  ieq(const char *a, const char *b) { while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; } return *a == *b; }
static int  iabs(int x) { return x < 0 ? -x : x; }
static int  clamp255(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }

/* ----------------------------- colour ---------------------------------- */
static int hexv(char c) { c = lc(c); return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:-1; }
static void hsl2rgb(int h, int s, int l, int *R, int *G, int *B) {
    h = ((h % 360) + 360) % 360; if (s<0)s=0; if(s>100)s=100; if(l<0)l=0; if(l>100)l=100;
    int c = (255*(100-iabs(2*l-100))*s)/10000, x = c*(60-iabs((h%120)-60))/60, m = 255*l/100 - c/2, r,g,b;
    if(h<60){r=c;g=x;b=0;}else if(h<120){r=x;g=c;b=0;}else if(h<180){r=0;g=c;b=x;}
    else if(h<240){r=0;g=x;b=c;}else if(h<300){r=x;g=0;b=c;}else{r=c;g=0;b=x;}
    *R=clamp255(r+m);*G=clamp255(g+m);*B=clamp255(b+m);
}
static int rdnum(const char **pp, int pct255) {
    const char *p=*pp; while(*p==' '||*p==','||*p=='\t')p++;
    int neg=0; if(*p=='-'){neg=1;p++;}else if(*p=='+')p++;
    int v=0,any=0; while(*p>='0'&&*p<='9'){v=v*10+(*p-'0');p++;any=1;}
    if(*p=='.'){p++;while(*p>='0'&&*p<='9')p++;}
    int pct=0; if(*p=='%'){pct=1;p++;}
    *pp=p; if(!any)return -1; if(neg)v=-v; if(pct&&pct255)v=v*255/100; return v;
}
uint32_t css_color(const char *v) {
    while(*v==' ')v++;
    if(v[0]=='#'){ const char*h=v+1; int n=0; while(hexv(h[n])>=0)n++;
        if(n>=6) return 0x1000000u|((uint32_t)(hexv(h[0])*16+hexv(h[1]))<<16)|((uint32_t)(hexv(h[2])*16+hexv(h[3]))<<8)|(uint32_t)(hexv(h[4])*16+hexv(h[5]));
        if(n>=3) return 0x1000000u|((uint32_t)(hexv(h[0])*17)<<16)|((uint32_t)(hexv(h[1])*17)<<8)|(uint32_t)(hexv(h[2])*17);
        return 0; }
    if((lc(v[0])=='r'&&lc(v[1])=='g'&&lc(v[2])=='b')||(lc(v[0])=='h'&&lc(v[1])=='s'&&lc(v[2])=='l')){
        int hsl=lc(v[0])=='h'; const char*p=v+3; if(lc(*p)=='a')p++; while(*p==' ')p++; if(*p!='(')return 0; p++;
        if(hsl){int h=rdnum(&p,0),s=rdnum(&p,0),l=rdnum(&p,0); if(h<0&&s<0)return 0; int R,G,B; hsl2rgb(h<0?0:h,s,l,&R,&G,&B); return 0x1000000u|((uint32_t)R<<16)|((uint32_t)G<<8)|(uint32_t)B;}
        int r=rdnum(&p,1),g=rdnum(&p,1),b=rdnum(&p,1); if(r<0||g<0||b<0)return 0; return 0x1000000u|((uint32_t)clamp255(r)<<16)|((uint32_t)clamp255(g)<<8)|(uint32_t)clamp255(b);
    }
    char low[20]; uint32_t i=0; while(v[i]&&v[i]!=' '&&v[i]!=';'&&i<sizeof(low)-1){low[i]=lc(v[i]);i++;} low[i]=0;
    if(ieq(low,"transparent")||ieq(low,"none")||ieq(low,"inherit")||ieq(low,"initial")||ieq(low,"currentcolor"))return 0;
    struct { const char*n; uint32_t c; } nm[] = {
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xE04040},{"green",0x008000},{"blue",0x5070E0},
        {"yellow",0xE0E040},{"orange",0xE0A040},{"purple",0x800080},{"gray",0x808080},{"grey",0x808080},
        {"silver",0xC0C0C0},{"navy",0x000080},{"teal",0x008080},{"maroon",0x800000},{"lime",0x00FF00},
        {"aqua",0x00FFFF},{"fuchsia",0xFF00FF},{"olive",0x808000},{"pink",0xFFC0CB},{"gold",0xFFD700},
        {"cyan",0x40D0E0},{"magenta",0xE040E0},{"brown",0xA07040},{"indigo",0x4B0082},{"violet",0xEE82EE},
        {"crimson",0xDC143C},{"coral",0xFF7F50},{"salmon",0xFA8072},{"khaki",0xF0E68C},{"tan",0xD2B48C},
        {"beige",0xF5F5DC},{"ivory",0xFFFFF0},{"azure",0xF0FFFF},{"lavender",0xE6E6FA},{"turquoise",0x40E0D0},
        {"tomato",0xFF6347},{"darkblue",0x00008B},{"darkgreen",0x006400},{"darkred",0x8B0000},{"darkgray",0xA9A9A9},
        {"lightgray",0xD3D3D3},{"lightblue",0xADD8E6},{"steelblue",0x4682B4},{"royalblue",0x4169E1},{"skyblue",0x87CEEB},
        {"dodgerblue",0x1E90FF},{"slategray",0x708090},{"gainsboro",0xDCDCDC},{"whitesmoke",0xF5F5F5},
        {"hotpink",0xFF69B4},{"rebeccapurple",0x663399},{"forestgreen",0x228B22},{"goldenrod",0xDAA520},
    };
    for(uint32_t k=0;k<sizeof(nm)/sizeof(nm[0]);k++) if(ieq(low,nm[k].n)) return 0x1000000u|nm[k].c;
    return 0;
}

/* ----------------------------- length parsing -------------------------- */
/* px length; em/rem*16; % returns -1 here (resolved against parent later via
 * a sentinel); "auto" -> -1. Returns px or -1. */
static int css_px(const char *v) {
    while(*v==' ')v++;
    if(lc(v[0])=='a') return -1;                       /* auto */
    int neg=0; if(*v=='-'){neg=1;v++;}
    int n=0,any=0; while(*v>='0'&&*v<='9'){n=n*10+(*v-'0');v++;any=1;}
    if(*v=='.'){v++;while(*v>='0'&&*v<='9')v++;}
    if(!any) return -1;
    if((v[0]|32)=='e'&&(v[1]|32)=='m') n*=16;
    if(neg)n=-n;
    return n;
}
static uint8_t fs_scale(const char *v) {
    while(*v==' ')v++;
    if((*v>='0'&&*v<='9')||*v=='.'){int n=0,any=0;const char*p=v;while(*p>='0'&&*p<='9'){n=n*10+(*p-'0');p++;any=1;}if(*p=='.'){p++;while(*p>='0'&&*p<='9')p++;}if(!any)return 0;
        if(*p=='%')return n>=200?3:n>=125?2:1; if((p[0]|32)=='e'&&(p[1]|32)=='m')return n>=2?3:n>=1?2:1; return n>=30?3:n>=19?2:1;}
    char a=v[0]|32; if(a=='x')return 3; if(a=='l')return 2; if(a=='s'||a=='m')return 1; return 0;
}

/* ----------------------------- declarations ---------------------------- */
enum { SET_COLOR=1<<0, SET_BG=1<<1, SET_DISP=1<<2, SET_TA=1<<3, SET_FS=1<<4,
       SET_BOLD=1<<5, SET_IT=1<<6, SET_MARGIN=1<<7, SET_PAD=1<<8, SET_BORDER=1<<9,
       SET_W=1<<10, SET_H=1<<11, SET_FDIR=1<<12, SET_JUST=1<<13, SET_ALIGN=1<<14,
       SET_GAP=1<<15, SET_GROW=1<<16, SET_GCOLS=1<<17 };
typedef struct {
    uint32_t set, color, bg; uint8_t disp, ta, fs, bold, it;
    int16_t margin[4], pad[4], border[4]; int32_t w, h;
    uint8_t fdir, just, align; int16_t gap, grow;
    int16_t gcols[GRID_MAX_COLS]; uint8_t ngcols;
} decls;

/* parse a grid-template-columns value into tracks: px (>=0) or -fr (<0). */
static void parse_tracks(const char *v, int16_t out[GRID_MAX_COLS], uint8_t *nout) {
    uint8_t n = 0; const char *p = v;
    while (*p && n < GRID_MAX_COLS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (ieq(p, "repeat") || (lc(p[0])=='r'&&lc(p[1])=='e'&&lc(p[2])=='p')) {
            while (*p && *p != '(') p++; if (*p=='(') p++;
            int cnt = 0; while (*p>='0'&&*p<='9') { cnt=cnt*10+(*p-'0'); p++; }
            while (*p && *p != ',') p++; if (*p==',') p++;
            while (*p == ' ') p++;
            const char *ts = p; while (*p && *p != ')') p++;            /* single track in repeat */
            char tok[16]; uint32_t k=0; for (const char*q=ts;q<p&&k<15;q++) if(*q!=' ')tok[k++]=*q; tok[k]=0;
            int val; if ((tok[0]>='0'&&tok[0]<='9')) { int num=0; const char*r=tok; while(*r>='0'&&*r<='9'){num=num*10+(*r-'0');r++;} val=((r[0]|32)=='f'&&(r[1]|32)=='r')? -num : num; }
            else val = -1;
            for (int c=0;c<cnt && n<GRID_MAX_COLS;c++) out[n++]=(int16_t)val;
            if (*p==')') p++;
            continue;
        }
        const char *ts = p; while (*p && *p!=' '&&*p!='\t') p++;
        char tok[16]; uint32_t k=0; for(const char*q=ts;q<p&&k<15;q++)tok[k++]=*q; tok[k]=0;
        int val;
        if (ieq(tok,"auto")) val = -1;
        else if (tok[0]>='0'&&tok[0]<='9') { int num=0; const char*r=tok; while(*r>='0'&&*r<='9'){num=num*10+(*r-'0');r++;} val = ((r[0]|32)=='f'&&(r[1]|32)=='r') ? -num : num; }
        else val = -1;
        out[n++] = (int16_t)val;
    }
    *nout = n;
}

static void shorthand4(const char *v, int16_t out[4]) {
    int vals[4]; int n = 0; const char *p = v;
    while (*p && n < 4) {
        while(*p==' '||*p=='\t')p++; if(!*p)break;
        const char *s=p; while(*p&&*p!=' '&&*p!='\t')p++;
        char tmp[16]; uint32_t k=0; for(const char*q=s;q<p&&k<15;q++)tmp[k++]=*q; tmp[k]=0;
        vals[n++]=css_px(tmp)<0?0:css_px(tmp);
    }
    if(n==1){out[0]=out[1]=out[2]=out[3]=vals[0];}
    else if(n==2){out[0]=out[2]=vals[0];out[1]=out[3]=vals[1];}
    else if(n==3){out[0]=vals[0];out[1]=out[3]=vals[1];out[2]=vals[2];}
    else if(n>=4){out[0]=vals[0];out[1]=vals[1];out[2]=vals[2];out[3]=vals[3];}
}
/* first px length found in a value string (for "border: 1px solid #ccc") */
static int first_px(const char *v) {
    for(const char*p=v;*p;p++) if(*p>='0'&&*p<='9') return css_px(p);
    return 1;
}
static void parse_decls(const char *d, uint32_t n, decls *o) {
    uint32_t i=0;
    while(i<n){
        while(i<n&&(d[i]==';'||d[i]==' '||d[i]=='\t'||d[i]=='\n'||d[i]=='\r'))i++;
        char prop[24]; uint32_t pl=0;
        while(i<n&&d[i]!=':'&&d[i]!=';'&&pl<sizeof(prop)-1){char c=lc(d[i]);if(c!=' ')prop[pl++]=c;i++;}
        prop[pl]=0;
        if(i>=n||d[i]!=':'){while(i<n&&d[i]!=';')i++;continue;}
        i++;
        char val[96]; uint32_t vl=0; while(i<n&&d[i]!=';'&&vl<sizeof(val)-1)val[vl++]=d[i++]; val[vl]=0;
        char *v=val; while(*v==' ')v++;
        if(ieq(prop,"color")){uint32_t c=css_color(v);if(c){o->color=c;o->set|=SET_COLOR;}}
        else if(ieq(prop,"background")||ieq(prop,"background-color")){uint32_t c=css_color(v);o->bg=c;o->set|=SET_BG;}
        else if(ieq(prop,"display")){o->set|=SET_DISP; char a=lc(v[0]),b=lc(v[1]);
            o->disp = a=='n'?DISP_NONE : (a=='g')?DISP_GRID : (a=='f'&&b=='l')?DISP_FLEX : a=='i'?(ieq(v,"inline-block")?DISP_INLINE_BLOCK:DISP_INLINE) : DISP_BLOCK;}
        else if(ieq(prop,"text-align")){o->set|=SET_TA; char a=lc(v[0]); o->ta=a=='c'?TA_CENTER:a=='r'?TA_RIGHT:TA_LEFT;}
        else if(ieq(prop,"font-size")){uint8_t s=fs_scale(v);if(s){o->fs=s;o->set|=SET_FS;}}
        else if(ieq(prop,"font-weight")){o->set|=SET_BOLD;o->bold=(lc(v[0])=='b'||(v[0]>='6'&&v[0]<='9'))?1:0;}
        else if(ieq(prop,"font-style")){o->set|=SET_IT;o->it=(lc(v[0])=='i'||lc(v[0])=='o')?1:0;}
        else if(ieq(prop,"margin")){shorthand4(v,o->margin);o->set|=SET_MARGIN;}
        else if(ieq(prop,"margin-top")){o->margin[0]=css_px(v)<0?0:css_px(v);o->set|=SET_MARGIN;}
        else if(ieq(prop,"margin-right")){o->margin[1]=css_px(v)<0?0:css_px(v);o->set|=SET_MARGIN;}
        else if(ieq(prop,"margin-bottom")){o->margin[2]=css_px(v)<0?0:css_px(v);o->set|=SET_MARGIN;}
        else if(ieq(prop,"margin-left")){o->margin[3]=css_px(v)<0?0:css_px(v);o->set|=SET_MARGIN;}
        else if(ieq(prop,"padding")){shorthand4(v,o->pad);o->set|=SET_PAD;}
        else if(ieq(prop,"padding-top")){o->pad[0]=css_px(v)<0?0:css_px(v);o->set|=SET_PAD;}
        else if(ieq(prop,"padding-right")){o->pad[1]=css_px(v)<0?0:css_px(v);o->set|=SET_PAD;}
        else if(ieq(prop,"padding-bottom")){o->pad[2]=css_px(v)<0?0:css_px(v);o->set|=SET_PAD;}
        else if(ieq(prop,"padding-left")){o->pad[3]=css_px(v)<0?0:css_px(v);o->set|=SET_PAD;}
        else if(ieq(prop,"border")||ieq(prop,"border-width")){int w=first_px(v);for(int k=0;k<4;k++)o->border[k]=(int16_t)w;o->set|=SET_BORDER;}
        else if(ieq(prop,"width")){o->w=css_px(v);o->set|=SET_W;}
        else if(ieq(prop,"height")){o->h=css_px(v);o->set|=SET_H;}
        else if(ieq(prop,"flex-direction")){o->fdir=(lc(v[0])=='c')?FLEX_COLUMN:FLEX_ROW;o->set|=SET_FDIR;}
        else if(ieq(prop,"justify-content")){o->set|=SET_JUST;
            o->just = ieq(v,"center")?JUST_CENTER : (lc(v[0])=='f'&&ieq(v,"flex-end"))?JUST_END :
                      ieq(v,"space-between")?JUST_BETWEEN : ieq(v,"space-around")?JUST_AROUND :
                      ieq(v,"end")?JUST_END : JUST_START;}
        else if(ieq(prop,"align-items")){o->set|=SET_ALIGN;
            o->align = ieq(v,"center")?ALIGN_CENTER : ieq(v,"flex-end")||ieq(v,"end")?ALIGN_END :
                       ieq(v,"flex-start")||ieq(v,"start")?ALIGN_START : ALIGN_STRETCH;}
        else if(ieq(prop,"gap")||ieq(prop,"grid-gap")||ieq(prop,"grid-column-gap")||ieq(prop,"row-gap")||ieq(prop,"column-gap")){int g=css_px(v);o->gap=(int16_t)(g<0?0:g);o->set|=SET_GAP;}
        else if(ieq(prop,"grid-template-columns")){parse_tracks(v,o->gcols,&o->ngcols);if(o->ngcols)o->set|=SET_GCOLS;}
        else if(ieq(prop,"flex-grow")){int n=0;for(const char*p=v;*p>='0'&&*p<='9';p++)n=n*10+(*p-'0');o->grow=(int16_t)n;o->set|=SET_GROW;}
        else if(ieq(prop,"flex")){int n=0,any=0;const char*p=v;while(*p==' ')p++;while(*p>='0'&&*p<='9'){n=n*10+(*p-'0');p++;any=1;}o->grow=(int16_t)(any?n:1);o->set|=SET_GROW;}
    }
}

/* ----------------------------- stylesheet ------------------------------ */
#define LRULE_MAX 1024
typedef struct { char sel[128]; int spec, order; decls d; } lrule;

static int parse_sheet(const char *css, uint32_t len, lrule *rules, int cap) {
    int nr = 0; uint32_t i = 0; int order = 0;
    while (i < len && nr < cap) {
        while(i<len&&(css[i]==' '||css[i]=='\n'||css[i]=='\t'||css[i]=='\r'))i++;
        if(i>=len)break;
        if(css[i]=='@'){int depth=0; while(i<len){char c=css[i++]; if(c==';'&&depth==0)break; if(c=='{')depth++; if(c=='}'){if(--depth<=0)break;}} continue;}
        uint32_t s0=i; while(i<len&&css[i]!='{'&&css[i]!='}')i++; if(i>=len||css[i]=='}')break;
        uint32_t s1=i; i++;
        uint32_t b0=i; while(i<len&&css[i]!='}')i++; uint32_t b1=i; if(i<len)i++;
        decls d; memset(&d,0,sizeof d); d.w=d.h=-1;
        parse_decls(css+b0, b1-b0, &d);
        if(!d.set) continue;
        /* split selector list by ',' into separate rules (each its own specificity) */
        uint32_t p=s0;
        while(p<s1 && nr<cap){
            uint32_t q=p; while(q<s1&&css[q]!=',')q++;
            uint32_t a=p,b=q; while(a<b&&(css[a]==' '||css[a]=='\n'||css[a]=='\t'||css[a]=='\r'))a++;
            while(b>a&&(css[b-1]==' '||css[b-1]=='\n'||css[b-1]=='\t'||css[b-1]=='\r'))b--;
            if(b>a){ lrule*r=&rules[nr]; uint32_t o=0; for(uint32_t k=a;k<b&&o<sizeof(r->sel)-1;k++)r->sel[o++]=css[k]; r->sel[o]=0;
                r->spec=dom_specificity(r->sel); r->order=order; r->d=d; nr++; }
            p=q+1;
        }
        order++;
    }
    return nr;
}

/* ----------------------------- computed style -------------------------- */
static uint8_t default_disp(const char *tag) {
    if(ieq(tag,"head")||ieq(tag,"script")||ieq(tag,"style")||ieq(tag,"title")||ieq(tag,"meta")||ieq(tag,"link")) return DISP_NONE;
    if(ieq(tag,"span")||ieq(tag,"a")||ieq(tag,"b")||ieq(tag,"strong")||ieq(tag,"i")||ieq(tag,"em")||
       ieq(tag,"code")||ieq(tag,"small")||ieq(tag,"label")||ieq(tag,"cite")||ieq(tag,"sub")||ieq(tag,"sup")||
       ieq(tag,"u")||ieq(tag,"mark")||ieq(tag,"abbr")||ieq(tag,"q")||ieq(tag,"time")||ieq(tag,"var")) return DISP_INLINE;
    if(ieq(tag,"img")||ieq(tag,"input")||ieq(tag,"button")||ieq(tag,"select")||ieq(tag,"textarea")) return DISP_INLINE_BLOCK;
    if(ieq(tag,"li")) return DISP_LIST_ITEM;
    return DISP_BLOCK;
}
static void intrinsic(const char *tag, computed_style *cs) {
    if(tag[0]=='h'&&tag[1]>='1'&&tag[1]<='6'&&!tag[2]){ cs->bold=1; cs->fscale = tag[1]<='2'?3:2;
        cs->margin[0]=cs->margin[2]= tag[1]<='2'?12:8; }
    else if(ieq(tag,"b")||ieq(tag,"strong")) cs->bold=1;
    else if(ieq(tag,"i")||ieq(tag,"em")||ieq(tag,"cite")) cs->italic=1;
    else if(ieq(tag,"p")){ cs->margin[0]=cs->margin[2]=10; }
    else if(ieq(tag,"ul")||ieq(tag,"ol")){ cs->margin[0]=cs->margin[2]=8; cs->padding[3]=24; }
    else if(ieq(tag,"blockquote")){ cs->margin[0]=cs->margin[2]=10; cs->margin[3]=24; }
    else if(ieq(tag,"a")){ cs->color=0x1000000u|0x1A73E8; }
    else if(ieq(tag,"body")){ cs->margin[0]=cs->margin[1]=cs->margin[2]=cs->margin[3]=8; }
}
static void compute_style(dom_node *n, const computed_style *parent, lrule *rules, int nr, computed_style *cs) {
    memset(cs, 0, sizeof *cs); cs->width=cs->height=-1; cs->fscale=1;
    if(parent){ cs->color=parent->color; cs->fscale=parent->fscale; cs->bold=parent->bold; cs->italic=parent->italic; cs->talign=parent->talign; }
    else cs->color = 0x1000000u|0x202124;
    cs->display = default_disp(n->tag);
    intrinsic(n->tag, cs);
    /* replaced elements get an intrinsic box from width/height attrs */
    if(ieq(n->tag,"img")){ const char*w=dom_attr_get(n,"width"),*h=dom_attr_get(n,"height");
        int wv=0,hv=0; for(const char*p=w;p&&*p>='0'&&*p<='9';p++)wv=wv*10+(*p-'0');
        for(const char*p=h;p&&*p>='0'&&*p<='9';p++)hv=hv*10+(*p-'0');
        cs->width = wv>0?wv:120; cs->height = hv>0?hv:80; cs->display=DISP_INLINE_BLOCK; }
    else if(ieq(n->tag,"input")||ieq(n->tag,"button")||ieq(n->tag,"select")){
        if(cs->width<0)cs->width=160; if(cs->height<0)cs->height=24; cs->display=DISP_INLINE_BLOCK; cs->border[0]=cs->border[1]=cs->border[2]=cs->border[3]=1; }
    /* cascade: collect matching author rules, selection-sort by (spec, order),
     * apply lowest-priority first so higher specificity / later source wins. */
    int idx[64]; int m=0;
    for(int k=0;k<nr && m<64;k++) if(dom_matches(n, rules[k].sel)) idx[m++]=k;
    for(int i=0;i<m;i++){ int sel=i; for(int j=i+1;j<m;j++){
            lrule*A=&rules[idx[j]], *B=&rules[idx[sel]];
            if(A->spec<B->spec || (A->spec==B->spec && A->order<B->order)) sel=j; }
        int t=idx[i]; idx[i]=idx[sel]; idx[sel]=t; }
    for(int i=0;i<m;i++){
        decls*d=&rules[idx[i]].d;
        if(d->set&SET_COLOR)cs->color=d->color;
        if(d->set&SET_BG)cs->bg=d->bg;
        if(d->set&SET_DISP)cs->display=d->disp;
        if(d->set&SET_TA)cs->talign=d->ta;
        if(d->set&SET_FS)cs->fscale=d->fs;
        if(d->set&SET_BOLD)cs->bold=d->bold;
        if(d->set&SET_IT)cs->italic=d->it;
        if(d->set&SET_MARGIN)for(int k=0;k<4;k++)cs->margin[k]=d->margin[k];
        if(d->set&SET_PAD)for(int k=0;k<4;k++)cs->padding[k]=d->pad[k];
        if(d->set&SET_BORDER)for(int k=0;k<4;k++)cs->border[k]=d->border[k];
        if(d->set&SET_W)cs->width=d->w;
        if(d->set&SET_H)cs->height=d->h;
        if(d->set&SET_FDIR)cs->flex_dir=d->fdir;
        if(d->set&SET_JUST)cs->justify=d->just;
        if(d->set&SET_ALIGN)cs->align=d->align;
        if(d->set&SET_GAP)cs->gap=d->gap;
        if(d->set&SET_GROW)cs->flex_grow=d->grow;
        if(d->set&SET_GCOLS){cs->ngcols=d->ngcols;for(int k=0;k<d->ngcols;k++)cs->gcols[k]=d->gcols[k];}
    }
    /* inline style="" wins over author rules */
    const char *inl = dom_attr_get(n, "style");
    if(inl){ decls d; memset(&d,0,sizeof d); d.w=d.h=-1; parse_decls(inl,(uint32_t)strlen(inl),&d);
        if(d.set&SET_COLOR)cs->color=d.color; if(d.set&SET_BG)cs->bg=d.bg;
        if(d.set&SET_DISP)cs->display=d.disp; if(d.set&SET_TA)cs->talign=d.ta;
        if(d.set&SET_FS)cs->fscale=d.fs; if(d.set&SET_BOLD)cs->bold=d.bold; if(d.set&SET_IT)cs->italic=d.it;
        if(d.set&SET_MARGIN)for(int k=0;k<4;k++)cs->margin[k]=d.margin[k];
        if(d.set&SET_PAD)for(int k=0;k<4;k++)cs->padding[k]=d.pad[k];
        if(d.set&SET_BORDER)for(int k=0;k<4;k++)cs->border[k]=d.border[k];
        if(d.set&SET_W)cs->width=d.w; if(d.set&SET_H)cs->height=d.h;
        if(d.set&SET_FDIR)cs->flex_dir=d.fdir; if(d.set&SET_JUST)cs->justify=d.just;
        if(d.set&SET_ALIGN)cs->align=d.align; if(d.set&SET_GAP)cs->gap=d.gap;
        if(d.set&SET_GROW)cs->flex_grow=d.grow;
        if(d.set&SET_GCOLS){cs->ngcols=d.ngcols;for(int k=0;k<d.ngcols;k++)cs->gcols[k]=d.gcols[k];}
    }
}

/* ----------------------------- box tree -------------------------------- */
static layout_box *mk_box(LAr *a, dom_node *n) {
    layout_box *b = (layout_box *)la_alloc(a, sizeof(layout_box));
    if(!b) return 0; memset(b,0,sizeof *b); b->node=n; b->link=-1; return b;
}
static void box_add(layout_box *p, layout_box *c) {
    c->parent=p; c->next=0; if(p->last_child)p->last_child->next=c; else p->first_child=c; p->last_child=c;
}
/* recursively build boxes with computed styles; skip display:none */
static layout_box *build_box(LAr *a, dom_node *n, const computed_style *parent, lrule *rules, int nr) {
    if(n->type==DOM_TEXT){
        /* whitespace-only text already filtered by dom_parse */
        layout_box *b=mk_box(a,0); if(!b)return 0; b->is_text=1; b->text=n->text;
        if(parent)b->st=*parent; return b;
    }
    computed_style cs; compute_style(n, parent, rules, nr, &cs);
    if(cs.display==DISP_NONE) return 0;
    layout_box *b=mk_box(a,n); if(!b)return 0; b->st=cs;
    for(dom_node *c=n->first_child;c;c=c->next){
        layout_box *cb=build_box(a,c,&cs,rules,nr);
        if(cb) box_add(b,cb);
    }
    return b;
}

/* ----------------------------- layout ---------------------------------- */
static int line_h_of(uint8_t fs) { return fs==3?40:fs==2?28:18; }
int layout_line_height(int scale) { return line_h_of((uint8_t)scale); }
static int (*g_measure)(const char *, int, int) = 0;
void layout_set_text_measurer(int (*fn)(const char *, int, int)) { g_measure = fn; }
static int text_w(const char *s, int len, uint8_t fs) {
    if (g_measure) return g_measure(s, len, fs?fs:1);
    return len * 8 * (fs?fs:1);
}

static int is_inline(layout_box *b) {
    if(b->is_text) return 1;
    return b->st.display==DISP_INLINE;
}

/* Lay out a run of inline boxes [items...] starting at (x0,y0) within width w.
 * Returns the height consumed. Sets geometry on each inline/text box. */
static int layout_inline_run(layout_box **items, int count, int x0, int y0, int w, uint8_t talign) {
    int pen_x=x0, pen_y=y0, line_h=0, line_start=0;
    /* track per-line for centering: simple left only for now unless talign set */
    for(int i=0;i<count;i++){
        layout_box *it=items[i];
        uint8_t fs = it->st.fscale?it->st.fscale:1;
        int lh = line_h_of(fs);
        const char *s = it->is_text ? it->text : "";
        /* split text into words; inline elements measured as one chunk of their text */
        if(it->is_text){
            it->x=pen_x; it->y=pen_y; it->h=lh;
            int sp = text_w(" ",1,fs);
            const char *p=s;
            while(*p){
                while(*p==' '||*p=='\n'||*p=='\t'||*p=='\r')p++;
                if(!*p)break;
                const char *wstart=p; int wl=0; while(*p&&*p!=' '&&*p!='\n'&&*p!='\t'&&*p!='\r'){p++;wl++;}
                int ww=text_w(wstart,wl,fs);
                int space = (pen_x>x0)?sp:0;
                if(pen_x>x0 && pen_x+space+ww > x0+w){ pen_x=x0; pen_y+=line_h?line_h:lh; line_h=0; space=0; (void)line_start;}
                pen_x+=space+ww;
                if(lh>line_h)line_h=lh;
            }
            it->w = pen_x - it->x; if(it->w<0)it->w=0;
        } else {
            /* inline element: place its text content inline (its children are inline) */
            int chw=0; for(layout_box *c=it->first_child;c;c=c->next){ if(c->is_text) chw+=text_w(c->text,(int)strlen(c->text),fs);}
            if(chw==0) chw=8*fs;
            if(pen_x>x0 && pen_x+chw>x0+w){ pen_x=x0; pen_y+=line_h?line_h:lh; line_h=0; }
            it->x=pen_x; it->y=pen_y; it->w=chw; it->h=lh; it->cx=it->x; it->cy=it->y; it->cw=chw; it->ch=lh;
            /* position its inline children at the same spot (flow) */
            int cxp=it->x; for(layout_box *c=it->first_child;c;c=c->next){ c->x=cxp; c->y=pen_y; c->h=lh; if(c->is_text){c->w=text_w(c->text,(int)strlen(c->text),fs);cxp+=c->w;} }
            pen_x+=chw; if(lh>line_h)line_h=lh;
        }
    }
    (void)talign;
    return (pen_y - y0) + (line_h?line_h:0);
}

static int layout_block(layout_box *box, int x, int y, int avail_w);

/* translate a whole box subtree by (dx,dy) */
static void shift_box(layout_box *b, int dx, int dy) {
    b->x += dx; b->y += dy; b->cx += dx; b->cy += dy;
    for (layout_box *c = b->first_child; c; c = c->next) shift_box(c, dx, dy);
}
/* outer (margin-box) main/cross sizes of a flex item */
static int item_outer_w(layout_box *it) { return it->is_text ? it->w : it->st.margin[3]+it->w+it->st.margin[1]; }
static int item_outer_h(layout_box *it) { return it->is_text ? it->h : it->st.margin[0]+it->h+it->st.margin[2]; }
static void measure_item(layout_box *it, int avail) {
    if (it->is_text) { uint8_t fs = it->st.fscale?it->st.fscale:1; it->x=it->y=0; it->h=line_h_of(fs); it->w=text_w(it->text,(int)strlen(it->text),fs); }
    else layout_block(it, 0, 0, avail);
}
/* max-content (single-line) border-box width of a subtree -- a flex-basis
 * estimate so an auto-width flex item doesn't greedily fill the container. */
static int maxc_w(layout_box *b) {
    if (b->is_text) { uint8_t fs = b->st.fscale?b->st.fscale:1; return text_w(b->text,(int)strlen(b->text),fs); }
    computed_style *s = &b->st;
    int pads = s->padding[3]+s->padding[1]+s->border[3]+s->border[1];
    if (s->width >= 0) return s->width + pads;
    int sum = 0, mx = 0;
    for (layout_box *c = b->first_child; c; c = c->next) {
        int cw = maxc_w(c) + (c->is_text ? 0 : c->st.margin[3]+c->st.margin[1]);
        if (c->is_text || c->st.display == DISP_INLINE) sum += cw;
        else if (cw > mx) mx = cw;
    }
    int inner = sum > mx ? sum : mx;
    return inner + pads;
}

/* Flexbox layout of a display:flex container (geometry already set on `box`).
 * Supports row/column, flex-grow, gap, justify-content, align-items. Returns
 * the content height consumed. */
static int layout_flex(layout_box *box) {
    computed_style *s = &box->st;
    layout_box *it[128]; int n = 0;
    for (layout_box *c = box->first_child; c && n < 128; c = c->next) it[n++] = c;
    if (n == 0) return 0;
    int gap = s->gap;
    int row = (s->flex_dir != FLEX_COLUMN);
    int main_avail = row ? box->cw : (box->ch >= 0 ? box->ch : 100000);

    for (int i = 0; i < n; i++) {
        /* auto-width row items size to content (flex-basis), not the container */
        if (row && !it[i]->is_text && it[i]->st.width < 0) {
            int bw = maxc_w(it[i]);
            int cw = bw - it[i]->st.padding[3]-it[i]->st.padding[1]-it[i]->st.border[3]-it[i]->st.border[1];
            it[i]->st.width = cw < 0 ? 0 : cw;
        }
        measure_item(it[i], box->cw);
    }

    /* flex-grow: distribute remaining main space among grow items (re-layout) */
    int used = 0, grow = 0;
    for (int i = 0; i < n; i++) { used += row ? item_outer_w(it[i]) : item_outer_h(it[i]); grow += it[i]->st.flex_grow; }
    used += gap * (n - 1);
    int remaining = main_avail - used; if (remaining < 0) remaining = 0;
    if (row && grow > 0 && remaining > 0) {
        for (int i = 0; i < n; i++) if (it[i]->st.flex_grow > 0 && !it[i]->is_text) {
            int extra = remaining * it[i]->st.flex_grow / grow;
            int contentw = it[i]->w - it[i]->st.padding[3]-it[i]->st.padding[1]-it[i]->st.border[3]-it[i]->st.border[1];
            it[i]->st.width = contentw + extra;
            layout_block(it[i], 0, 0, box->cw);
        }
        used = 0; for (int i = 0; i < n; i++) used += item_outer_w(it[i]); used += gap*(n-1);
    }

    /* cross extent */
    int cross = 0;
    for (int i = 0; i < n; i++) { int oc = row ? item_outer_h(it[i]) : item_outer_w(it[i]); if (oc > cross) cross = oc; }

    /* main-axis placement (justify-content) */
    int free = main_avail - used; if (free < 0 || main_avail >= 100000) free = 0;
    int start = 0, spacing = 0;
    if (s->justify == JUST_CENTER) start = free/2;
    else if (s->justify == JUST_END) start = free;
    else if (s->justify == JUST_BETWEEN && n > 1) spacing = free/(n-1);
    else if (s->justify == JUST_AROUND) { spacing = free/n; start = spacing/2; }

    int cur = (row ? box->cx : box->cy) + start;
    for (int i = 0; i < n; i++) {
        int ow = item_outer_w(it[i]), oh = item_outer_h(it[i]);
        int omain = row ? ow : oh, ocross = row ? oh : ow;
        int coff = 0;
        if (s->align == ALIGN_CENTER) coff = (cross - ocross)/2;
        else if (s->align == ALIGN_END) coff = cross - ocross;
        int ml = it[i]->is_text ? 0 : it[i]->st.margin[3], mt = it[i]->is_text ? 0 : it[i]->st.margin[0];
        int tx, ty;
        if (row) { tx = cur + ml; ty = box->cy + coff + mt; }
        else     { tx = box->cx + coff + ml; ty = cur + mt; }
        shift_box(it[i], tx - it[i]->x, ty - it[i]->y);
        cur += omain + gap + spacing;
    }
    return row ? cross : (cur - box->cy);
}

/* CSS Grid layout (geometry already set on `box`): resolves grid-template-columns
 * (px + fr tracks), places children row-major into cells, auto-sizes row heights.
 * Returns content height consumed. */
static int layout_grid(layout_box *box) {
    computed_style *s = &box->st;
    int ncol = s->ngcols > 0 ? s->ngcols : 1;
    int gap = s->gap;
    int colw[GRID_MAX_COLS]; int fixed = 0, fr = 0;
    for (int i = 0; i < ncol; i++) { if (s->gcols[i] >= 0) { colw[i] = s->gcols[i]; fixed += colw[i]; } else { fr += -s->gcols[i]; colw[i] = 0; } }
    int avail = box->cw - gap*(ncol-1) - fixed; if (avail < 0) avail = 0;
    if (fr > 0) for (int i = 0; i < ncol; i++) if (s->gcols[i] < 0) colw[i] = avail * (-s->gcols[i]) / fr;
    int colx[GRID_MAX_COLS]; int xx = box->cx; for (int i = 0; i < ncol; i++) { colx[i] = xx; xx += colw[i] + gap; }

    int row_y = box->cy, rowh = 0, idx = 0;
    for (layout_box *c = box->first_child; c; c = c->next) {
        int col = idx % ncol;
        if (col == 0 && idx > 0) { row_y += rowh + gap; rowh = 0; }
        measure_item(c, colw[col]);
        /* laid out at origin: element border-box sits at (margin_l, margin_t), text at (0,0);
         * translating by (colx, row_y) lands it in the cell with margins preserved. */
        shift_box(c, colx[col], row_y);
        int oh = item_outer_h(c); if (oh > rowh) rowh = oh;
        idx++;
    }
    row_y += rowh;
    return row_y - box->cy;
}

/* Block layout of `box` whose containing-block content starts at (x,y) with
 * content width avail_w. Returns total vertical space consumed (incl margins). */
static int layout_block(layout_box *box, int x, int y, int avail_w) {
    computed_style *s=&box->st;
    int ml=s->margin[3], mr=s->margin[1], mt=s->margin[0], mb=s->margin[2];
    int bl=s->border[3], br=s->border[1], bt=s->border[0], bb=s->border[2];
    int pl=s->padding[3], pr=s->padding[1], pt=s->padding[0], pb=s->padding[2];

    int border_w = (s->width>=0) ? (s->width + pl+pr+bl+br) : (avail_w - ml - mr);
    if(border_w < 0) border_w = 0;
    int bx = x + ml, by = y + mt;
    box->x=bx; box->y=by; box->w=border_w;
    box->cx = bx + bl + pl; box->cy = by + bt + pt;
    box->cw = border_w - bl - br - pl - pr; if(box->cw<0)box->cw=0;

    int cursor = box->cy;
    if (s->display == DISP_FLEX) {
        cursor = box->cy + layout_flex(box);
    } else if (s->display == DISP_GRID) {
        cursor = box->cy + layout_grid(box);
    } else {
        /* group children: flush consecutive inline boxes together, block ones recurse */
        layout_box *inl[256]; int ni=0;
        for(layout_box *c=box->first_child;c;c=c->next){
            if(is_inline(c)){
                if(ni<256) inl[ni++]=c;
            } else {
                if(ni>0){ cursor += layout_inline_run(inl,ni,box->cx,cursor,box->cw,s->talign); ni=0; }
                int adv = layout_block(c, box->cx, cursor, box->cw);
                cursor += adv;
            }
        }
        if(ni>0){ cursor += layout_inline_run(inl,ni,box->cx,cursor,box->cw,s->talign); ni=0; }
    }

    int content_h = cursor - box->cy;
    if(s->height>=0) content_h = s->height;
    box->ch = content_h;
    box->h = content_h + pt+pb+bt+bb;
    if(s->height>=0) box->h = s->height + pt+pb+bt+bb;
    return mt + box->h + mb;
}

layout_tree *layout_build(dom_document *doc, const char *css, uint32_t csslen, int viewport_w) {
    if(!doc) return 0;
    LImpl *im=(LImpl*)kmalloc(sizeof(LImpl)); if(!im)return 0;
    im->ar.head=0; im->ar.total=0; im->ar.oom=0;
    LAr *a=&im->ar;
    lrule *rules=(lrule*)kmalloc(sizeof(lrule)*LRULE_MAX);
    int nr = (rules&&css&&csslen)? parse_sheet(css,csslen,rules,LRULE_MAX):0;

    layout_box *root=build_box(a, doc->root, 0, rules, nr);
    if(rules) kfree(rules);
    if(!root || a->oom){ la_free(a); kfree(im); return 0; }
    im->t.root=root; im->t.arena=a;
    /* the document root is the initial containing block */
    root->st.display=DISP_BLOCK; root->st.width=-1;
    int total = layout_block(root, 0, 0, viewport_w);
    im->t.doc_w=viewport_w; im->t.doc_h=total;
    return &im->t;
}
void layout_free(layout_tree *t) {
    if(!t) return; LImpl*im=(LImpl*)t; la_free(&im->ar); kfree(im);
}
