/* tables_3d.c - 3D surface camera / picking math (see tables_3d.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include "tables_3d.h"

#define T3D_PITCH_MIN 0.08
#define T3D_PITCH_MAX 1.45
#define T3D_ZOOM_MIN 0.25
#define T3D_ZOOM_MAX 4.0
#define T3D_PAN_MAX 12.0

void lt_t3d_reset_view(lt_t3d *t)
{
    t->yaw = 0.6; t->pitch = 0.55; t->zoom = 1.0; t->panx = 0.0; t->pany = 0.0;
}

void lt_t3d_init(lt_t3d *t)
{
    lt_t3d_reset_view(t);
    t->style = LT_T3D_SOLID; t->color_mode = LT_T3D_BY_VALUE; t->grid = 1;
}

void lt_t3d_orbit(lt_t3d *t, double dyaw, double dpitch)
{
    t->yaw += dyaw;
    t->pitch += dpitch;
    if (t->pitch < T3D_PITCH_MIN) { t->pitch = T3D_PITCH_MIN; }
    if (t->pitch > T3D_PITCH_MAX) { t->pitch = T3D_PITCH_MAX; }
}

void lt_t3d_zoom(lt_t3d *t, double factor)
{
    if (!(factor > 0.0)) { return; }
    t->zoom *= factor;
    if (t->zoom < T3D_ZOOM_MIN) { t->zoom = T3D_ZOOM_MIN; }
    if (t->zoom > T3D_ZOOM_MAX) { t->zoom = T3D_ZOOM_MAX; }
}

void lt_t3d_wheel(lt_t3d *t, int notches)
{
    int i, n = notches < 0 ? -notches : notches;
    for (i = 0; i < n && i < 20; i++) { lt_t3d_zoom(t, notches > 0 ? 0.9 : 1.0 / 0.9); }
}

void lt_t3d_pan(lt_t3d *t, double dx, double dy, int side)
{
    double k = 19.0 * (t->zoom > 0.2 ? t->zoom : 1.0) / (double)(side > 1 ? side : 1);
    t->panx -= dx * k;
    t->pany += dy * k;
    if (t->panx < -T3D_PAN_MAX) { t->panx = -T3D_PAN_MAX; }
    if (t->panx > T3D_PAN_MAX) { t->panx = T3D_PAN_MAX; }
    if (t->pany < -T3D_PAN_MAX) { t->pany = -T3D_PAN_MAX; }
    if (t->pany > T3D_PAN_MAX) { t->pany = T3D_PAN_MAX; }
}

void lt_cam3_setup(const lt_t3d *t, int x, int y, int w, int h, lt_cam3 *c)
{
    double tx = LT_T3D_W * 0.5, ty = LT_T3D_H * 0.35, tz = LT_T3D_W * 0.5;
    double dist = 19.0 * (t->zoom > 0.2 ? t->zoom : 1.0);
    double cp = cos(t->pitch), sp = sin(t->pitch), cyw = cos(t->yaw), syw = sin(t->yaw);
    double fl, rl, ex0, ey0, ez0;
    ex0 = tx + dist * cp * syw; ey0 = ty + dist * sp; ez0 = tz + dist * cp * cyw;
    c->fx = tx - ex0; c->fy = ty - ey0; c->fz = tz - ez0;
    fl = sqrt(c->fx * c->fx + c->fy * c->fy + c->fz * c->fz);
    c->fx /= fl; c->fy /= fl; c->fz /= fl;
    /* right = forward x worldUp(0,1,0) */
    c->rx = -c->fz; c->ry = 0.0; c->rz = c->fx;
    rl = sqrt(c->rx * c->rx + c->ry * c->ry + c->rz * c->rz);
    if (rl < 1e-9) { c->rx = 1.0; c->ry = 0.0; c->rz = 0.0; rl = 1.0; }
    c->rx /= rl; c->ry /= rl; c->rz /= rl;
    /* up = right x forward */
    c->ux = c->ry * c->fz - c->rz * c->fy;
    c->uy = c->rz * c->fx - c->rx * c->fz;
    c->uz = c->rx * c->fy - c->ry * c->fx;
    /* pan moves eye and target together */
    c->ex = ex0 + c->rx * t->panx + c->ux * t->pany;
    c->ey = ey0 + c->ry * t->panx + c->uy * t->pany;
    c->ez = ez0 + c->rz * t->panx + c->uz * t->pany;
    c->f = (double)(h < w ? h : w);
    c->cx = (double)x + (double)w * 0.5;
    c->cy = (double)y + (double)h * 0.5;
}

double lt_cam3_proj(const lt_cam3 *c, double x, double y, double z, double *sx, double *sy)
{
    double vx = x - c->ex, vy = y - c->ey, vz = z - c->ez;
    double xc = vx * c->rx + vy * c->ry + vz * c->rz;
    double yc = vx * c->ux + vy * c->uy + vz * c->uz;
    double zc = vx * c->fx + vy * c->fy + vz * c->fz;
    if (zc < 0.1) { zc = 0.1; }
    *sx = c->cx + c->f * xc / zc;
    *sy = c->cy - c->f * yc / zc;
    return zc;
}

void lt_t3d_world(int rows, int cols, int ybottom, double zlo, double span, int r, int c, double v,
                  double *wx, double *wy, double *wz)
{
    double rr = rows > 1 ? (double)r / (double)(rows - 1) : 0.0;
    double cc = cols > 1 ? (double)c / (double)(cols - 1) : 0.0;
    *wx = cc * LT_T3D_W;
    *wz = ybottom ? (1.0 - rr) * LT_T3D_W : rr * LT_T3D_W;
    *wy = span > 0.0 ? (v - zlo) / span * LT_T3D_H : 0.0;
}

int lt_t3d_pick(const lt_cam3 *cam, int rows, int cols, int ybottom, const double *z, double zlo,
                double span, int mx, int my, double radius, int *r, int *c)
{
    int i, j, found = 0;
    double best = radius * radius, bestdepth = 1e300;
    for (i = 0; i < rows; i++) {
        for (j = 0; j < cols; j++) {
            double wx, wy, wz, sx, sy, dx, dy, d2, depth;
            lt_t3d_world(rows, cols, ybottom, zlo, span, i, j, z[(size_t)i * (size_t)cols + (size_t)j],
                         &wx, &wy, &wz);
            depth = lt_cam3_proj(cam, wx, wy, wz, &sx, &sy);
            dx = sx - (double)mx; dy = sy - (double)my;
            d2 = dx * dx + dy * dy;
            if (d2 < best || (found && d2 <= best && depth < bestdepth)) {
                best = d2; bestdepth = depth; found = 1; *r = i; *c = j;
            }
        }
    }
    return found;
}
