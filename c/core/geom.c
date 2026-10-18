/* 几何工具 —— 与 server.py 中 haversine / seg_point_dist 逐行等价的移植。
 * 数值行为（含浮点求值顺序）尽量与 Python 版一致，保证两端结果对齐。
 */
#include "geom.h"

#include <math.h>

/* 地球半径（米），同 server.py EARTH_R；-std=c17 下无 M_PI，自备 */
#define CRTR_EARTH_R 6371008.8
static const double CRTR_PI = 3.14159265358979323846;

/* 角度转弧度（同 math.radians） */
static double crtr_rad(double deg)
{
    return deg * (CRTR_PI / 180.0);
}

double crtr_haversine(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = crtr_rad(lat1), p2 = crtr_rad(lat2);
    double dp = p2 - p1;
    double dl = crtr_rad(lon2 - lon1);
    double a = pow(sin(dp / 2.0), 2.0) +
               cos(p1) * cos(p2) * pow(sin(dl / 2.0), 2.0);
    return 2.0 * CRTR_EARTH_R * asin(sqrt(a));
}

double crtr_seg_point_dist(double lat, double lon,
                           double alat, double alon,
                           double blat, double blon)
{
    /* 点到线段的最短距离（米），用局部等距投影近似。
     * Python 版以 (lat,lon) 为原点，故 px=py=0（原 (lon-lon)/(lat-lat) 恒为 0）。
     */
    double coslat = cos(crtr_rad(lat));
    double kx = 111320.0 * coslat;
    double ky = 110540.0;
    double px = 0.0, py = 0.0;
    double ax = (alon - lon) * kx, ay = (alat - lat) * ky;
    double bx = (blon - lon) * kx, by = (blat - lat) * ky;
    double dx = bx - ax, dy = by - ay;
    double L2 = dx * dx + dy * dy;
    double t, cx, cy;

    if (L2 <= 0.0)
        return hypot(px - ax, py - ay);
    t = ((px - ax) * dx + (py - ay) * dy) / L2;
    if (t < 0.0)
        t = 0.0;
    else if (t > 1.0)
        t = 1.0;
    cx = ax + t * dx;
    cy = ay + t * dy;
    return hypot(px - cx, py - cy);
}
