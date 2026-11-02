// Included first in every gl4es source on Apple platforms (iOS build).
// gl4es's AliasDecl gives a function a second name with an ELF alias, which
// Mach-O does not have; here it only declares the second name, and
// gl4es_apple.c defines those functions as calls to the first.
#define AliasDecl(RET,NAME,DEF,OLD) RET APIENTRY_GL4ES NAME DEF

// gl4es's limits (its src/config.h, which this replaces through its include
// guard), with more program local parameters: Sega Race TV's precompiled
// vertex programs use program.local up to 164, its fragment programs up to 49;
// and more vertex env parameters: OutRun 2 SP's NV vertex programs read c[]
// up to 212 (NV_vertex_program2 has 256; gl4es kept 96 and dropped the rest)
#define _GL4ES_MAIN_CONFIG_H
#define SYS_proxy 9999
#define MAX_EVAL_ORDER 30
#define MAX_TEX 16
#define MAX_LIGHT 8
#define MAX_STACK_PROJECTION 16
#define MAX_STACK_TEXTURE 16
#define MAX_STACK_MODELVIEW 64
#define MAX_STACK_COLOR 16
#define MAX_STACK_ARB_MATRIX 8
#define MAX_CLIP_PLANES 6
#define MAX_VATTRIB 32
#define MAX_MAP_SIZE 256
#define MAX_ARB_MATRIX 8
#define MAX_VTX_PROG_ENV_PARAMS 256
#define MAX_VTX_PROG_LOC_PARAMS 192
#define MAX_FRG_PROG_ENV_PARAMS 24
#define MAX_FRG_PROG_LOC_PARAMS 64
#define MAX_DRAW_BUFFERS 16
