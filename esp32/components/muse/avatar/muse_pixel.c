// Copyright (c) 2026 Dani Martinez - dan1t0
/* Snorlax: a sleepy teal companion with a cream belly, pointed ears, tiny
 * fangs and padded feet. Original procedural character drawing; display
 * scaling follows the SDK renderer. All animation stays on the 64x64 grid. */
#include "muse_pixel.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
enum { BG, INK, TEAL_D, TEAL, TEAL_L, TEAL_H, CREAM_D, CREAM, CREAM_L,
       PAD, PAD_L, MOUTH, TONGUE, WHITE, BLUSH, SHADOW, GLOW_D, GLOW,
       SPARK, HEART, C_COUNT };
static const uint32_t COLORS[C_COUNT] = {
    0x000000, 0x102a34, 0x245461, 0x347887, 0x4893a0, 0x6aadb4,
    0xc7b486, 0xe6d6a6, 0xffedc3, 0x83694c, 0xa78b61,
    0x352d35, 0xdd7c87, 0xfff8df, 0xc99186, 0x0b1721,
    0x10232a, 0x79c4b8, 0xe3eee0, 0xef94ad
};
static const uint32_t ACCENTS[MUSE_MODE_COUNT] = {
    0xb8dfef, 0x79b7c5, 0x79caff, 0xd7adf2, 0x83dfba, 0xf48d94, 0x829ab7
};
static uint8_t s_fb[W*H], s_mask[W*H];
static uint16_t s_pal[C_COUNT], s_pal_dim[C_COUNT];
static const uint8_t BAYER[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
static float s_last_t = -1, s_accent[3];
static bool s_init;
static int clampi(int n, int a, int b) { return n<a?a:n>b?b:n; }
static float clampf(float n, float a, float b) { return n<a?a:n>b?b:n; }
static int rnd(float n) { return (int)floorf(n+0.5f); }
static void px(int x,int y,int c) { if((unsigned)x<W&&(unsigned)y<H)s_fb[y*W+x]=(uint8_t)c; }
static uint16_t rgb565(int r,int g,int b) { return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3)); }
uint32_t muse_pixel_accent(muse_mode_t mode) { return ACCENTS[(unsigned)mode<MUSE_MODE_COUNT?mode:MUSE_MODE_IDLE]; }
static void palette(muse_mode_t mode,float dt,float fade) {
    uint32_t acc=muse_pixel_accent(mode); float a=1-expf(-7*dt);
    for(int c=0;c<3;c++) {
        float target=(float)((acc>>(16-8*c))&255);
        s_accent[c]=s_init?s_accent[c]+a*(target-s_accent[c]):target;
    }
    s_init=true;
    for(int i=0;i<C_COUNT;i++) {
        int r=(COLORS[i]>>16)&255,g=(COLORS[i]>>8)&255,b=COLORS[i]&255;
        if(i==GLOW||i==GLOW_D||i==SPARK) {
            float scale=i==GLOW_D?0.13f:i==GLOW?0.7f:1;
            r=(int)(s_accent[0]*scale);g=(int)(s_accent[1]*scale);b=(int)(s_accent[2]*scale);
        }
        r=(int)(r*fade);g=(int)(g*fade);b=(int)(b*fade);
        s_pal[i]=rgb565(r,g,b);s_pal_dim[i]=rgb565(r*72/100,g*72/100,b*72/100);
    }
}
static void line(int x0,int y0,int x1,int y1,int c) {
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;
    for(;;) { px(x0,y0,c);if(x0==x1&&y0==y1)break;int e2=2*e;
        if(e2>=dy){e+=dy;x0+=sx;}if(e2<=dx){e+=dx;y0+=sy;} }
}
static bool inside(int x,int y,int rx,int ry) {
    return x*x*ry*ry+y*y*rx*rx<=rx*rx*ry*ry;
}
static void ellipse(int cx,int cy,int rx,int ry,int color,bool outline) {
    for(int y=-ry;y<=ry;y++)for(int x=-rx;x<=rx;x++)if(inside(x,y,rx,ry)) {
        int c=color;
        if(outline&&(!inside(x-1,y,rx,ry)||!inside(x+1,y,rx,ry)||!inside(x,y-1,rx,ry)||!inside(x,y+1,rx,ry)))c=INK;
        else if(color==TEAL||color==CREAM) {
            int light=8-x*5/(rx?rx:1)-y*6/(ry?ry:1);
            if(light>BAYER[(y+cy)&3][(x+cx)&3]+7)c=color+1;
            else if(light+7<BAYER[(y+cy)&3][(x+cx)&3])c=color-1;
        }
        px(cx+x,cy+y,c);
    }
}
static void mask_ellipse(int cx,int cy,int rx,int ry) {
    for(int y=-ry;y<=ry;y++)for(int x=-rx;x<=rx;x++)
        if(inside(x,y,rx,ry)&&(unsigned)(cx+x)<W&&(unsigned)(cy+y)<H)s_mask[(cy+y)*W+cx+x]=1;
}
static void ear(int tipx,int top,int basex,int bottom) {
    for(int y=top;y<=bottom;y++) {
        int width=1+(y-top)*5/(bottom-top),cx=tipx+(basex-tipx)*(y-top)/(bottom-top);
        for(int x=cx-width;x<=cx+width;x++)if((unsigned)x<W&&(unsigned)y<H)s_mask[y*W+x]=1;
    }
}
static int mask(int x,int y) { return (unsigned)x<W&&(unsigned)y<H?s_mask[y*W+x]:0; }
static void body(int cx,int cy,int heady,int wide) {
    memset(s_mask,0,sizeof(s_mask));
    mask_ellipse(cx,cy,wide,17);mask_ellipse(cx,heady,15,12);
    ear(cx-13,heady-16,cx-10,heady-6);ear(cx+13,heady-16,cx+10,heady-6);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++)if(mask(x,y)) {
        int c=TEAL,light=11-(x-cx)/3-(y-heady)/5;
        if(!mask(x-1,y)||!mask(x+1,y)||!mask(x,y-1)||!mask(x,y+1))c=INK;
        else if(light>BAYER[y&3][x&3]+4)c=TEAL_L;
        else if(light+6<BAYER[y&3][x&3])c=TEAL_D;
        px(x,y,c);
    }
    ellipse(cx,cy+1,wide-5,13,CREAM,false);
    /* The two cream cheeks leave Snorlax's characteristic teal forehead point. */
    ellipse(cx-6,heady+1,7,8,CREAM,false);ellipse(cx+6,heady+1,7,8,CREAM,false);
    ellipse(cx,heady+6,12,6,CREAM,false);
}
static void star(int x,int y,int color) {
    px(x,y,color);px(x-1,y,GLOW);px(x+1,y,GLOW);px(x,y-1,GLOW);px(x,y+1,GLOW);
}
static void heart(int x,int y) {
    static const char *rows[]={".##.##.","#######","#######",".#####.","..###..","...#..."};
    for(int r=0;r<6;r++)for(int c=0;c<7;c++)if(rows[r][c]=='#')px(x+c,y+r,HEART);
}
static void foot(int x,int y) {
    ellipse(x,y,8,6,CREAM,true);ellipse(x,y+1,4,3,PAD,false);px(x-1,y,PAD_L);
    for(int k=-1;k<=1;k++) { int xx=x+k*4;px(xx,y-4,WHITE);px(xx,y-5,WHITE);px(xx+1,y-4,CREAM_L); }
}
static void eye(int x,int y,int expression,int look) {
    if(expression==2) {line(x-2,y-2,x+2,y+2,INK);line(x+2,y-2,x-2,y+2,INK);}
    else if(expression==1) {line(x-2,y,x,y-2,INK);line(x,y-2,x+2,y,INK);}
    else if(expression==3) {ellipse(x,y,2,2,INK,false);px(x+look,y-1,WHITE);}
    else {line(x-2,y,x+2,y,INK);px(x-3,y-1,INK);px(x+3,y-1,INK);}
}

void muse_pixel_render(const muse_pose_t *p) {
    muse_mode_t mode=(unsigned)p->mode<MUSE_MODE_COUNT?p->mode:MUSE_MODE_IDLE;
    float t=p->t,mt=p->mode_t,dt=s_last_t<0?0.04f:clampf(t-s_last_t,0,0.2f);s_last_t=t;
    float level=clampf(p->level,0,1),happy=mode==MUSE_MODE_ERROR?0:clampf(p->happy,0,1);
    float fade=mode==MUSE_MODE_OFF?clampf(1-mt/2,0.15f,1):1;
    palette(mode,dt,fade);memset(s_fb,0,sizeof(s_fb));
    int hop=happy>0?rnd(fabsf(sinf(t*8))*3*happy):0;
    int bob=rnd(sinf(t*1.5f)*0.6f)-hop;
    int lean=mode==MUSE_MODE_THINKING?rnd(sinf(t*2)*1.1f):0;
    if(mode==MUSE_MODE_ERROR&&mt<0.6f)lean=rnd(sinf(mt*65)*2);
    if(mode==MUSE_MODE_BOOT)bob+=rnd((1-clampf(mt/0.6f,0,1))*7);
    int cx=32+lean,cy=39+bob,hy=22+bob,wide=20+(sinf(t*1.5f)>0.7f);
    if(mode==MUSE_MODE_SPEAKING)cy-=rnd(level);
    /* Quiet backdrop, with the screen's black background left untouched. */
    for(int y=3;y<61;y++)for(int x=3;x<61;x++) {
        int d=(x-32)*(x-32)+(y-31)*(y-31);
        if(d>600&&d<760&&BAYER[y&3][x&3]<3)px(x,y,GLOW_D);
    }
    for(int y=57;y<=60;y++)for(int x=17;x<=47;x++)if(inside(x-32,(y-59)*4,16,6)&&((x+y)&1))px(x,y,SHADOW);
    for(int k=0;k<3;k++) {
        float a=t*(mode==MUSE_MODE_THINKING?0.7f:0.2f)+k*2.094f;
        star(rnd(32+27*cosf(a)),rnd(30+23*sinf(a)),SPARK);
    }
    int arm_y=cy-1;
    if(mode==MUSE_MODE_LISTENING||happy>0.2f)arm_y=hy+6+rnd(sinf(t*7));
    int right_y=mode==MUSE_MODE_OFF?hy+3+rnd(sinf(t*10)*2):arm_y;
    if(mode==MUSE_MODE_SPEAKING)right_y-=rnd(level*4);
    ellipse(cx-19,arm_y,6,9,TEAL,true);ellipse(cx+19,right_y,6,9,TEAL,true);
    body(cx,cy,hy,wide);
    if(mode==MUSE_MODE_THINKING)ellipse(cx+10,hy+12,5,6,TEAL,true);
    for(int side=-1;side<=1;side+=2) {
        int yy=side<0?arm_y:right_y;
        if(mode==MUSE_MODE_LISTENING||happy>0.2f||mode==MUSE_MODE_OFF) {
            for(int k=0;k<3;k++) {px(cx+side*21+k-1,yy-7,WHITE);px(cx+side*21+k-1,yy-6,CREAM);}
        }
    }
    foot(cx-12,54+bob);foot(cx+12,54+bob+(mode==MUSE_MODE_SPEAKING?rnd(sinf(t*6)*0.6f):0));
    int expression=happy>0.2f?1:mode==MUSE_MODE_ERROR?2:mode==MUSE_MODE_LISTENING?3:0;
    if(mode==MUSE_MODE_BOOT&&mt>0.9f&&mt<1.5f)expression=3;
    if(mode==MUSE_MODE_OFF)expression=0;
    eye(cx-6,hy+1,expression,0);eye(cx+6,hy+1,expression,0);
    int my=hy+6;
    if(mode==MUSE_MODE_SPEAKING||happy>0.2f) {
        int height=happy>0.2f?3:1+rnd(level*3);
        ellipse(cx,my+1,4,height,MOUTH,false);if(height>1)line(cx-2,my+height,cx+2,my+height,TONGUE);
    } else if(mode==MUSE_MODE_LISTENING)ellipse(cx,my+1,2,2,MOUTH,false);
    else {line(cx-4,my,cx+4,my,INK);if(mode!=MUSE_MODE_ERROR){px(cx-5,my-1,INK);px(cx+5,my-1,INK);}}
    if(mode!=MUSE_MODE_ERROR) {px(cx-4,my-1,WHITE);px(cx-4,my,WHITE);px(cx+4,my-1,WHITE);px(cx+4,my,WHITE);}
    if(happy>0.2f) {line(cx-11,hy+4,cx-9,hy+4,BLUSH);line(cx+9,hy+4,cx+11,hy+4,BLUSH);heart(cx-25,hy-15-rnd(sinf(t*3)));heart(cx+17,hy-18+rnd(sinf(t*3)));}
    if(mode==MUSE_MODE_THINKING)for(int k=0;k<3;k++)ellipse(cx+18+k*3,hy-8-k*4,1,1,((int)(t*3)%3)==k?WHITE:GLOW,false);
    if(mode==MUSE_MODE_ERROR) {line(cx+22,hy-10,cx+22,hy-5,HEART);px(cx+22,hy-2,HEART);}
    if(mode==MUSE_MODE_IDLE&&happy<0.1f) {
        int x=cx+18,y=hy-13-rnd(fmodf(t,3));
        line(x,y,x+4,y,GLOW);line(x+4,y,x,y+4,GLOW);line(x,y+4,x+4,y+4,GLOW);
    }
    if(mode==MUSE_MODE_LISTENING||mode==MUSE_MODE_SPEAKING)for(int side=-1;side<=1;side+=2)for(int k=0;k<2;k++) {
        int x=cx+side*(27+k*3),h=1+rnd(level*4)+k;line(x,hy+3-h,x,hy+3+h,k?GLOW_D:GLOW);
    }
}

/* SDK display-strip scaler: never allocates a full panel-sized frame. */
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;
void muse_pixel_set_size(int px) {
    s_size=clampi(px,1,MAP_MAX);bool grid=s_size>=3*W;
    for(int i=0;i<s_size;i++){int cell=i*W/s_size;bool edge=grid&&(i+1)*W/s_size!=cell;s_map[i]=(uint8_t)(cell|(edge?0x80:0));}
}
void muse_pixel_scale(uint16_t *dst,int stride_px,int x0,int x1,int y0,int y1) {
    for(int y=y0;y<=y1;y++) {
        uint8_t my=s_map[y];uint16_t *out=dst+(y-y0)*stride_px;
        if(y>y0&&my==s_map[y-1]) {memcpy(out,out-stride_px,(size_t)(x1-x0+1)*sizeof(*out));continue;}
        for(int x=x0;x<=x1;x++){uint8_t mx=s_map[x];int c=s_fb[(my&0x7f)*W+(mx&0x7f)];out[x-x0]=((mx|my)&0x80)?s_pal_dim[c]:s_pal[c];}
    }
}
