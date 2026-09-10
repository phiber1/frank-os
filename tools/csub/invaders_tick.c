/* invaders_tick.c — per-frame mover/drawer CSUB for invaders.bas
 *
 * Call:  itick ct%, st%(), al%(), spr%()
 *   ct  = MM.INFO(CALLTABLE)
 *   st  = state array (indices below)
 *   al  = alive list: al(0) = count, al(1..) = c + r*16
 *   spr = sprite pixel store: 7 slots x 16 rows x 12 bytes (4bpp),
 *         filled via the GRAB command from the loading-screen art
 *
 * The CSUB draws straight into the FRANKOS gfx buffer (4bpp, high
 * nibble = left pixel) and marks one dirty rect per call via the
 * CallTable's basic_gfx_mark, so the display flush paints everything
 * as a single frame.  All x coords kept even by the BASIC side.
 */

typedef long long ll;
typedef void (*mark_fn)(int, int, int, int);

#define CT_HRES     19
#define CT_MARK     55
#define CT_WRITEBUF 56

/* st() indices */
#define ST_CMD   0   /* bit0 march, bit1 shot, bit2 bombs, bit3 ufo,
                        bit4 grab-sprite, bit5 grab-palette */
#define ST_GX    1   /* armada origin (updated by march) */
#define ST_GY    2
#define ST_DX    3   /* march delta for this call */
#define ST_DY    4
#define ST_ANIM  5
#define ST_SX    6   /* shot; SY < 0 = inactive */
#define ST_SY    7
#define ST_UX    8   /* ufo x; < 0 = inactive */
#define ST_USPD  9
#define ST_PLX   10  /* player x for bomb collision */
#define ST_EV    11  /* OUT: event bits */
#define ST_HC    12  /* OUT: hit cell col / GRAB slot / palette result */
#define ST_HR    13  /* OUT: hit cell row */
#define ST_MINC  14  /* OUT: leftmost alive column after march */
#define ST_MAXC  15  /* OUT: rightmost alive column after march */
#define ST_B0X   16  /* bombs; BY < 0 = inactive */
#define ST_B0Y   17
#define ST_WHITE 22  /* palette nibbles, set by BASIC via grab-palette */
#define ST_YELLOW 23

/* events */
#define EV_SHOTGONE  1
#define EV_SHIELD    2
#define EV_HITINV    4
#define EV_HITUFO    8
#define EV_INVADED   16
#define EV_PLAYERHIT 32
#define EV_UFOGONE   64
#define EV_BOMBGONE  128

/* geometry — must mirror invaders.bas */
#define COLS 10
#define ROWS 5
#define GW   40
#define GH   26
#define IW   24
#define IH   16
#define PY   336
#define UFOY 24

struct ctx {
    unsigned char *buf;
    int stride;
    int minx, miny, maxx, maxy;
    int white, yellow;
};

static void touch(struct ctx *g, int x1, int y1, int x2, int y2)
{
    if (x1 < g->minx) g->minx = x1;
    if (y1 < g->miny) g->miny = y1;
    if (x2 > g->maxx) g->maxx = x2;
    if (y2 > g->maxy) g->maxy = y2;
}

/* Framebuffer height (FRANKOS DVI 640x480 4bpp).  Every buffer write
 * below is CLAMPED to [0,stride) x [0,FBH): an out-of-range coordinate
 * used to scribble into OS memory past the framebuffer, and the
 * corruption accumulated until the whole system starved. */
#define FBH 480

/* fill rect; x and w even (byte-aligned nibble pairs) */
static void fill(struct ctx *g, int x, int y, int w, int h, int nib)
{
    unsigned char b = (unsigned char)((nib << 4) | nib);
    x &= ~1;
    int bx0 = x >> 1, bx1 = bx0 + (w >> 1);      /* byte span [bx0,bx1) */
    if (bx0 < 0) bx0 = 0;
    if (bx1 > g->stride) bx1 = g->stride;
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= FBH) continue;
        unsigned char *p = g->buf + yy * g->stride;
        for (int i = bx0; i < bx1; i++) p[i] = b;
    }
    touch(g, x, y, x + w - 1, y + h - 1);
}

/* read one pixel nibble (0 if out of bounds) */
static int pix(struct ctx *g, int x, int y)
{
    if (x < 0 || y < 0 || y >= FBH || (x >> 1) >= g->stride) return 0;
    unsigned char b = g->buf[y * g->stride + (x >> 1)];
    return (x & 1) ? (b & 0x0F) : (b >> 4);
}

/* blit sprite slot (12 bytes x 16 rows) at even x; nibble 0 opaque
 * black (the armada erase covers it, and rows are byte copies) */
static void blit(struct ctx *g, ll *spr, int slot, int x, int y, int h)
{
    unsigned char *src8 = (unsigned char *)spr + slot * 192;
    int bx = x >> 1;                              /* dest byte offset */
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= FBH) continue;
        unsigned char *p = g->buf + yy * g->stride;
        unsigned char *s = src8 + r * 12;
        for (int i = 0; i < 12; i++) {
            int bi = bx + i;
            if (bi >= 0 && bi < g->stride) p[bi] = s[i];
        }
    }
    touch(g, x, y, x + IW - 1, y + h - 1);
}

/* copy a 24-wide, h-tall region FROM the buffer into a sprite slot */
static void grab(struct ctx *g, ll *spr, int slot, int x, int y, int h)
{
    unsigned char *dst = (unsigned char *)spr + slot * 192;
    int bx = x >> 1;
    for (int r = 0; r < 16; r++) {
        int yy = y + r;
        for (int i = 0; i < 12; i++) {
            int bi = bx + i;
            dst[r * 12 + i] = (r < h && yy >= 0 && yy < FBH &&
                               bi >= 0 && bi < g->stride)
                              ? g->buf[yy * g->stride + bi] : 0;
        }
    }
}

long long main(ll *ct, ll *st, ll *al, ll *spr)
{
    void **T = (void **)(unsigned int)(*ct);
    struct ctx g;
    g.buf    = *(unsigned char **)T[CT_WRITEBUF];
    g.stride = (*(int *)T[CT_HRES]) >> 1;
    g.minx = g.miny = 100000;
    g.maxx = g.maxy = -1;
    g.white  = (int)st[ST_WHITE];
    g.yellow = (int)st[ST_YELLOW];

    int cmd = (int)st[ST_CMD];
    int ev  = 0;

    /* ── GRAB commands (load time) ── */
    if (cmd & 16) {                       /* grab sprite image */
        grab(&g, spr, (int)st[ST_HC], (int)st[ST_SX], (int)st[ST_SY],
             (int)st[ST_HR]);
        return 0;
    }
    if (cmd & 32) {                       /* grab palette nibble */
        st[ST_HC] = pix(&g, (int)st[ST_SX], (int)st[ST_SY]);
        return 0;
    }
    if (cmd & 64) {                       /* blit sprite slot at SX,SY */
        blit(&g, spr, (int)st[ST_HC], (int)st[ST_SX], (int)st[ST_SY],
             (int)st[ST_HR]);
        goto out;
    }

    int gx = (int)st[ST_GX], gy = (int)st[ST_GY];
    int anim = (int)st[ST_ANIM];
    int n = (int)al[0];

    /* ── MARCH: move the whole armada in one pass ── */
    if (cmd & 1) {
        int dx = (int)st[ST_DX], dy = (int)st[ST_DY];
        int minc = COLS, maxc = -1;
        for (int i = 1; i <= n; i++) {
            int c = (int)al[i] & 15, r = (int)al[i] >> 4;
            int ox = gx + c * GW, oy = gy + r * GH;
            fill(&g, ox, oy, IW, IH, 0);
            int typ = (r == 0) ? 0 : (r < 3) ? 1 : 2;
            blit(&g, spr, typ * 2 + anim, ox + dx, oy + dy, IH);
            if (oy + dy + IH >= PY) ev |= EV_INVADED;
            if (c < minc) minc = c;
            if (c > maxc) maxc = c;
        }
        gx += dx; gy += dy;
        st[ST_GX] = gx; st[ST_GY] = gy;
        st[ST_MINC] = minc; st[ST_MAXC] = maxc;
    }

    /* ── BOMBS ── */
    if (cmd & 4) {
        int plx = (int)st[ST_PLX];
        for (int b = 0; b < 3; b++) {
            int xi = ST_B0X + b * 2, yi = ST_B0Y + b * 2;
            int bx = (int)st[xi], by = (int)st[yi];
            if (by < 0) continue;
            fill(&g, bx, by, 2, 6, 0);
            by += 6;
            if (by + 6 >= 372) {           /* hit the earth line */
                fill(&g, bx - 2, 372, 4, 2, 0);
                by = -1; ev |= EV_BOMBGONE;
            } else if (by > 284 && by < 330 && pix(&g, bx, by + 6)) {
                fill(&g, bx - 4, by + 2, 8, 8, 0);
                by = -1; ev |= EV_BOMBGONE;
            } else if (by + 6 >= PY && bx >= plx && bx < plx + IW) {
                by = -1; ev |= EV_BOMBGONE | EV_PLAYERHIT;
            } else {
                fill(&g, bx, by, 2, 6, g.yellow);
            }
            st[yi] = by;
        }
    }

    /* ── UFO (USPD signed: passes run either direction) ── */
    if ((cmd & 8) && st[ST_UX] >= 0) {
        int ux = (int)st[ST_UX];
        fill(&g, ux, UFOY, IW, 10, 0);
        ux += (int)st[ST_USPD];
        if (ux > 606 || ux < 2) {
            ux = -1; ev |= EV_UFOGONE;
        } else {
            blit(&g, spr, 6, ux, UFOY, 10);
        }
        st[ST_UX] = ux;
    }

    /* ── SHOT ── */
    if ((cmd & 2) && st[ST_SY] != -1) {
        int sx = (int)st[ST_SX], sy = (int)st[ST_SY];
        if (sy == -2) {                    /* ceiling linger expired */
            fill(&g, sx, 20, 2, 8, 0);
            st[ST_SY] = -1; ev |= EV_SHOTGONE;
            goto shot_done;
        }
        if (sy >= 1000) {                  /* freshly fired: first draw
                                              at the muzzle, no erase/move */
            sy -= 1000;
            fill(&g, sx, sy, 2, 8, g.white);
            st[ST_SY] = sy;
            goto shot_done;
        }
        fill(&g, sx, sy, 2, 8, 0);
        sy -= 14;
        if (sy < 12) {
            /* draw one lingering frame at the ceiling so the full
             * height is always visible (a 1-frame top position lost a
             * race with the 25ms display flush ~1/3 of the time) */
            fill(&g, sx, 20, 2, 8, g.white);
            sy = -2;
        } else if (sy > 291 && sy < 321 &&
                   (pix(&g, sx, sy) || pix(&g, sx, sy + 4) || pix(&g, sx, sy + 7))) {
            fill(&g, sx - 4, sy, 8, 16, 0);   /* carve the barrier */
            sy = -1; ev |= EV_SHIELD;
        } else {
            /* invader collision against the (post-march) grid */
            int hit = 0;
            if (sx >= gx && sy >= gy) {
                int c = (sx - gx) / GW, r = (sy - gy) / GH;
                if (c < COLS && r < ROWS &&
                    sx < gx + c * GW + IW && sy < gy + r * GH + IH) {
                    for (int i = 1; i <= n; i++) {
                        if (((int)al[i] & 15) == c && ((int)al[i] >> 4) == r) {
                            blit(&g, spr, 7, gx + c * GW, gy + r * GH, IH);
                            st[ST_HC] = c; st[ST_HR] = r;
                            ev |= EV_HITINV; hit = 1; sy = -1;
                            break;
                        }
                    }
                }
            }
            if (!hit && st[ST_UX] >= 0 &&
                sy < UFOY + 10 && sy + 8 >= UFOY &&
                sx + 2 > st[ST_UX] && sx < st[ST_UX] + IW) {
                fill(&g, (int)st[ST_UX], UFOY, IW, 10, 0);
                st[ST_UX] = -1;
                ev |= EV_HITUFO; sy = -1;
            } else if (!hit && sy >= 0) {
                fill(&g, sx, sy, 2, 8, g.white);
            }
        }
        st[ST_SY] = sy;
shot_done: ;
    }

out:
    st[ST_EV] = ev;
    if (g.maxx >= 0) {
        /* clamp the dirty rect to the screen — touch() accumulates raw
         * (possibly out-of-range) coords, and the OS flush copies this
         * rect to the display, so an out-of-range rect makes the OS
         * itself read/write past the framebuffer */
        int x1 = g.minx, y1 = g.miny, x2 = g.maxx, y2 = g.maxy;
        int w = g.stride << 1;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > w - 1) x2 = w - 1;
        if (y2 > FBH - 1) y2 = FBH - 1;
        if (x2 >= x1 && y2 >= y1) {
            mark_fn mark = (mark_fn)T[CT_MARK];
            mark(x1, y1, x2, y2);
        }
    }
    return 0;
}
