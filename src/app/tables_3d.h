/* tables_3d.h - camera and picking model of the table editor's 3D surface view (G-26).
 * Pure math (no UI): orbit / zoom / pan state, the perspective camera, the world position of a
 * table vertex and the nearest-vertex pick used for click-to-select.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TABLES_3D_H
#define LT_TABLES_3D_H

#include "../lt.h"

#define LT_T3D_W 10.0          /* world size of the surface footprint */
#define LT_T3D_H 5.0           /* world height of the surface */

#define LT_T3D_SOLID  0        /* surface styles */
#define LT_T3D_WIRE   1
#define LT_T3D_POINTS 2
#define LT_T3D_STYLES 3
#define LT_T3D_BY_VALUE  0     /* colour modes */
#define LT_T3D_BY_CHANGE 1
#define LT_T3D_SOLIDCOL  2
#define LT_T3D_MODES  3

typedef struct lt_t3d {
    double yaw, pitch;         /* orbit angles (rad) */
    double zoom;               /* distance factor: smaller = closer */
    double panx, pany;         /* target offset along the camera right / up axes (world units) */
    int style, color_mode, grid;
} lt_t3d;

typedef struct lt_cam3 {
    double ex, ey, ez;                            /* eye */
    double rx, ry, rz, ux, uy, uz, fx, fy, fz;    /* right, up, forward */
    double f, cx, cy;                             /* focal length in px, screen centre */
} lt_cam3;

void lt_t3d_init(lt_t3d *t);          /* defaults (solid, by value, grid on, default view) */
void lt_t3d_reset_view(lt_t3d *t);    /* default orbit / zoom / pan; keeps style options */
void lt_t3d_orbit(lt_t3d *t, double dyaw, double dpitch);   /* pitch clamped 0.08..1.45 */
void lt_t3d_zoom(lt_t3d *t, double factor);                 /* multiplies the distance, clamped */
/* pan by a screen drag of (dx, dy) px in a view whose shorter side is `side` px */
void lt_t3d_pan(lt_t3d *t, double dx, double dy, int side);
/* wheel notches (+1 = in) */
void lt_t3d_wheel(lt_t3d *t, int notches);

void lt_cam3_setup(const lt_t3d *t, int x, int y, int w, int h, lt_cam3 *c);
/* world -> screen; returns the depth along the view direction */
double lt_cam3_proj(const lt_cam3 *c, double x, double y, double z, double *sx, double *sy);

/* world position of table vertex (row r, col c) of a rows x cols table whose value is v in
 * [zlo, zlo + span]; ybottom flips the row axis like the 2D grid */
void lt_t3d_world(int rows, int cols, int ybottom, double zlo, double span, int r, int c, double v,
                  double *wx, double *wy, double *wz);
/* nearest vertex to the screen point (mx, my) within `radius` px (nearer to the eye wins a
 * tie); 1 and (*r, *c) on a hit, else 0 */
int lt_t3d_pick(const lt_cam3 *cam, int rows, int cols, int ybottom, const double *z, double zlo,
                double span, int mx, int my, double radius, int *r, int *c);

#endif
