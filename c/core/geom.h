#ifndef CRTR_GEOM_H
#define CRTR_GEOM_H

/* 几何工具 —— 与 server.py 中 haversine / seg_point_dist 逐行等价 */

double crtr_haversine(double lat1, double lon1, double lat2, double lon2);
double crtr_seg_point_dist(double lat, double lon,
                           double alat, double alon,
                           double blat, double blon);

#endif
