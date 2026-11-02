// NV_vertex_program to ARB vertex programs (nv_vertex_program.c)
#ifndef NV_VERTEX_PROGRAM_H
#define NV_VERTEX_PROGRAM_H
#include <stddef.h>

#define NVVP_PARAMS 256  // c[0..255], as NV_vertex_program2 has
#define NVVP_TRACKS 64   // glTrackMatrixNV addresses, one per 4 parameters
typedef struct { unsigned address, matrix, transform; } nvvp_track;
// a malloc'd ARB program for the NV program src (len bytes)
char *nvvp_to_arb(const char *src, size_t len, const nvvp_track *track);
// a malloc'd ARB fragment program for the NV fragment program src
char *nvfp_to_arb(const char *src, size_t len);
#endif
