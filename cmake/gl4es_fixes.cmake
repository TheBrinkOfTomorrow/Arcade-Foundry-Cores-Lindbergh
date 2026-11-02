# Fixes to gl4es (MIT) at the pinned commit, applied to the fetched source
# at configure time (each only once: the replacement no longer matches):
#  - arbhelper.c resize(): the new capacity is a byte count, stored as the
#    element count, so a long ARB parameter list overran its array.
#  - shaderconv.c: program.local[] indices were read up to two digits, so
#    ARB programs with local parameters past 99 failed to compile.
#  - arbparser.c: the name made for state.light[n].<property> was allocated
#    too short ("gl_LightSource[n]." is 17 characters, not 8).
#  - hardext.c: on an OpenGL ES 3 context, packed depth+stencil, 24-bit
#    depth, RGBA8 renderbuffers and 32-bit indices are core and not listed as
#    extensions; gl4es looked for the extensions only (and split depth and
#    stencil, which Apple's GLES rejects). An ES 3 context allows more
#    varyings than ES 2 (HotD4's skinned shaders need them).
#  - fpe_shader.c: an ARB vertex program drawn with the fixed-function
#    fragment stage (OutRun 2 SP) was unfinished at this commit (a debug
#    printf, and none of the varyings gl4es's own fragment shader reads):
#    the vertex shader now writes Color, BackColor, SecColor, SecBackColor and
#    FogSrc from the program's outputs, and the fog coordinate whenever the
#    program does not; gl4es's own vertex shader drawn with an ARB fragment
#    program that reads the fog coordinate writes it too.
#  - program.c: the elements of a uniform array were taken to sit at
#    consecutive locations. Apple's GLES spaces them by the type's columns (a
#    mat4 array's elements 4 apart, a mat3 array's 3 apart), so an element set
#    on its own landed inside an earlier one: HotD4 sets its bone matrices one
#    by one, and its skinned enemies were drawn with spikes. Each element's
#    location is now asked from the driver.
#  - texture_read.c: glCopyTexSubImage2D into an RGBA texture is a framebuffer
#    blit on OpenGL ES 3 (done on the GPU); Apple's glCopyTexSubImage2D waited
#    for the GPU, about 7 ms a call on the iPad in HotD4's post effects.
#  - stack.c, stack.h, enable.c: glPushAttrib(GL_ENABLE_BIT) and glPopAttrib
#    left GL_VERTEX_PROGRAM_ARB and GL_FRAGMENT_PROGRAM_ARB out, and
#    glIsEnabled did not know GL_FRAGMENT_PROGRAM_ARB: OutRun 2 SP enables its
#    fragment program inside a push, and after the pop gl4es drew its
#    fixed-function screen composite through that program (a white screen).
#  - fpe.c: an ARB fragment program reading the secondary colour drawn with an
#    ARB vertex program that does not write it did not link on OpenGL ES
#    (OutRun 2 SP's attract); the vertex shader writes 0 there.
#  - gl4es.c: glVertexPointer, glColorPointer, glNormalPointer and the others
#    made with clone_gl_pointer add the bound buffer's data to the pointer but
#    kept the array's old buffer, so the draw added a buffer's data a second
#    time. OutRun 2 SP's car selector sets attribute 0 from a buffer
#    (glVertexAttribPointerARB) and then its shadow from client memory
#    (glVertexPointer): the shadow was read far past the game's memory and
#    the app crashed. The buffer is now cleared there.
function(gl4es_fix file from to)
  file(READ "${file}" text)
  string(FIND "${text}" "${from}" at)
  if(NOT at EQUAL -1)
    string(REPLACE "${from}" "${to}" text "${text}")
    file(WRITE "${file}" "${text}")
  endif()
endfunction()

gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/arbhelper.c"
  "		*obj = reloc;\n		*cap = newSize;"
  "		*obj = reloc;\n		*cap = newSize / esize;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/shaderconv.c"
  "          int n = (*p) - '0';\n          if(p[1]>='0' && p[1]<='9')\n            n = n*10 + (p[1] - '0');"
  "          int n = 0;\n          for(const char *q = p; *q>='0' && *q<='9'; ++q)\n            n = n*10 + (*q - '0');")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/arbparser.c"
  "malloc((mtxNameLen + strlen(sln) + 8) * sizeof(char));\n\t\t\t\t\tsprintf(matrixNameMallocd, \"gl_LightSource[%s].%s\""
  "malloc((strlen(matrixName) + strlen(sln) + 20) * sizeof(char));\n\t\t\t\t\tsprintf(matrixNameMallocd, \"gl_LightSource[%s].%s\"")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "    int color = default_fragment?(strstr(initial, \"_gl4es_Color\")?0:1):0;   // need to add a simple color variant?\nif(default_fragment) printf(\"fpe_CustomVertexShader(%p, %p, %d)\\n%s\\ncolor=%d\\n\", initial, state, default_fragment, initial, color);"
  "    int color = 0;\n    int fogvec = state->fogsource!=FPE_FOG_SRC_COORD && state->fogdist==FPE_FOG_DIST_RADIAL;\n    if(default_fragment) {   // the varyings fpe_FragmentShader reads\n        sprintf(buff, \"varying vec4 Color;\\nvarying vec4 BackColor;\\nvarying vec4 SecColor;\\nvarying vec4 SecBackColor;\\nvarying highp %s FogSrc;\\n\", fogvec?\"vec3\":\"float\");\n        ShadAppend(buff);\n        headline += 5;\n    }")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "    // wrap main if needed\n    if(planes || color) {"
  "    // wrap main if needed\n    if(planes || color || default_fragment) {")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "        ShadAppend(\"_gl4es_main();\\n\");\n        if(planes) {\n            int clipvertex = 0;"
  "        ShadAppend(\"_gl4es_main();\\n\");\n        if(default_fragment) {\n            ShadAppend(\"Color = gl_FrontColor;\\nBackColor = gl_BackColor;\\nSecColor = gl_FrontSecondaryColor;\\nSecBackColor = gl_BackSecondaryColor;\\n\");\n            if(!strstr(initial, \"gl_FogFragCoord\"))\n                ShadAppend(\"gl_FogFragCoord = abs(gl_Position.w);\\n\");\n            ShadAppend(fogvec?\"FogSrc = vec3(0.0, 0.0, gl_FogFragCoord);\\n\":\"FogSrc = gl_FogFragCoord;\\n\");\n        }\n        if(planes) {\n            int clipvertex = 0;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/glx/hardext.c"
  "    S(\"GL_OES_rgb8_rgba8 \", rgba8, 1);\n"
  "    S(\"GL_OES_rgb8_rgba8 \", rgba8, 1);\n    {\n        const char *ver = (const char *)gles_glGetString(GL_VERSION);\n        if (ver && strstr(ver, \"OpenGL ES 3\")) {\n            hardext.depthstencil = hardext.depth24 = hardext.rgba8 = hardext.elementuint = 1;\n            SHUT_LOGD(\"OpenGL ES 3: packed depth stencil, depth24, rgba8 and uint indices are core\\n\");\n        }\n    }\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/program.c"
  "                    if(gluniform->size==1) ++glprogram->num_uniform;\n                    id++;"
  "                    if(gluniform->size==1) ++glprogram->num_uniform;\n                    if(j+1<size) {   // the next element's own location\n                        char *elem = (char*)malloc(strlen(name)+16);\n                        sprintf(elem, \"%s[%d]\", name, j+1);\n                        GLint next = gles_glGetUniformLocation(glprogram->id, elem);\n                        free(elem);\n                        id = (next!=-1)?next:id+1;\n                    }")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/program.c"
  "                if(index>m->size) {\n                    res = -1;   // too big !\n                } else\n                    res += index;"
  "                if(index>=m->size) {\n                    res = -1;   // too big !\n                } else if(index) {   // elements need not be consecutive\n                    LOAD_GLES2(glGetUniformLocation);\n                    res = gles_glGetUniformLocation(glprogram->id, name);\n                }")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_read.c"
  "    glstate->vao->unpack = unpack;\n}\n\nvoid APIENTRY_GL4ES gl4es_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,\n"
  "    glstate->vao->unpack = unpack;\n}\n\n// OpenGL ES 3: the copy as a framebuffer blit into the texture, on the GPU. Apple's\n// glCopyTexSubImage2D waited for the GPU to finish the frame so far (about 7 ms a\n// call on an M4 iPad, HotD4's glare and blur copying its frame 3-6 times a frame).\ntypedef void (*cbb_blit_fn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);\ntypedef void (*cbb_fbtex_fn)(GLenum, GLenum, GLenum, GLuint, GLint);\ntypedef void (*cbb_genfb_fn)(GLsizei, GLuint *);\ntypedef void (*cbb_bindfb_fn)(GLenum, GLuint);\ntypedef void (*cbb_geti_fn)(GLenum, GLint *);\ntypedef GLboolean (*cbb_isen_fn)(GLenum);\ntypedef void (*cbb_cap_fn)(GLenum);\ntypedef const GLubyte *(*cbb_str_fn)(GLenum);\nstatic int copy_by_blit(GLenum target, gltexture_t *bound, GLint level, GLint xoffset, GLint yoffset,\n                        GLint x, GLint y, GLsizei width, GLsizei height) {\n    static int tried;\n    static cbb_blit_fn blit;\n    static cbb_fbtex_fn fbtex;\n    static cbb_genfb_fn genfb;\n    static cbb_bindfb_fn bindfb;\n    static cbb_geti_fn geti;\n    static cbb_isen_fn isen;\n    static cbb_cap_fn en, dis;\n    static GLuint fb;\n    if(!tried) {\n        tried = 1;\n        cbb_str_fn str = (cbb_str_fn)proc_address(gles, \"glGetString\");\n        const char *ver = str ? (const char *)str(GL_VERSION) : NULL;\n        if(ver && strstr(ver, \"OpenGL ES 3\")) {\n            fbtex = (cbb_fbtex_fn)proc_address(gles, \"glFramebufferTexture2D\");\n            genfb = (cbb_genfb_fn)proc_address(gles, \"glGenFramebuffers\");\n            bindfb = (cbb_bindfb_fn)proc_address(gles, \"glBindFramebuffer\");\n            geti = (cbb_geti_fn)proc_address(gles, \"glGetIntegerv\");\n            isen = (cbb_isen_fn)proc_address(gles, \"glIsEnabled\");\n            en = (cbb_cap_fn)proc_address(gles, \"glEnable\");\n            dis = (cbb_cap_fn)proc_address(gles, \"glDisable\");\n            if(fbtex && genfb && bindfb && geti && isen && en && dis)\n                blit = (cbb_blit_fn)proc_address(gles, \"glBlitFramebuffer\");\n        }\n    }\n    if(!blit || target!=GL_TEXTURE_2D || !bound || !bound->glname || bound->format!=GL_RGBA || bound->type!=GL_UNSIGNED_BYTE)\n        return 0;\n    if(!fb) genfb(1, &fb);\n    GLint cur = 0;\n    geti(GL_FRAMEBUFFER_BINDING, &cur);   // the framebuffer read from (readfboBegin)\n    GLboolean scissor = isen(GL_SCISSOR_TEST);\n    if(scissor) dis(GL_SCISSOR_TEST);\n    bindfb(0x8CA8, cur);   // GL_READ_FRAMEBUFFER\n    bindfb(0x8CA9, fb);    // GL_DRAW_FRAMEBUFFER\n    fbtex(0x8CA9, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bound->glname, level);\n    blit(x, y, x+width, y+height, xoffset, yoffset, xoffset+width, yoffset+height, GL_COLOR_BUFFER_BIT, GL_NEAREST);\n    fbtex(0x8CA9, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);\n    bindfb(GL_FRAMEBUFFER, cur);\n    if(scissor) en(GL_SCISSOR_TEST);\n    return 1;\n}\n\nvoid APIENTRY_GL4ES gl4es_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_read.c"
  "            gles_glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);\n            if(((((bound->max_level"
  "            if(!copy_by_blit(target, bound, level, xoffset, yoffset, x, y, width, height))\n                gles_glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);\n            if(((((bound->max_level")
# (the fog coordinate is written unless the program assigns it, not merely names it)
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "            if(!strstr(initial, \"gl_FogFragCoord\"))\n"
  "            if(!strstr(initial, \"gl_FogFragCoord =\") && !strstr(initial, \"gl_FogFragCoord=\"))\n")
# (and whatever the fragment stage, as an ARB fragment program reading the fog
# coordinate needs it as much: the vertex shader writes it unless it does)
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "    if(planes || color || default_fragment) {"
  "    int fogwrite = !strstr(initial, \"gl_FogFragCoord =\") && !strstr(initial, \"gl_FogFragCoord=\");\n    if(planes || color || default_fragment || fogwrite) {")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "            if(!strstr(initial, \"gl_FogFragCoord =\") && !strstr(initial, \"gl_FogFragCoord=\"))\n                ShadAppend(\"gl_FogFragCoord = abs(gl_Position.w);\\n\");\n"
  "")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "        ShadAppend(\"_gl4es_main();\\n\");\n        if(default_fragment) {"
  "        ShadAppend(\"_gl4es_main();\\n\");\n        if(fogwrite)\n            ShadAppend(\"gl_FogFragCoord = abs(gl_Position.w);\\n\");\n        if(default_fragment) {")
# gl4es's own vertex shader drawn with an ARB fragment program that reads the
# fog coordinate (OutRun 2 SP): it wrote only its FogSrc, not gl_FogFragCoord
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe_shader.c"
  "            default: sprintf(buff, \"FogSrc = vertex.z;\\n\");\n        }\n        ShadAppend(buff);\n        #endif"
  "            default: sprintf(buff, \"FogSrc = vertex.z;\\n\");\n        }\n        ShadAppend(buff);\n        if(need && need->need_fogcoord)\n            ShadAppend(fogsource==FPE_FOG_SRC_COORD?\"gl_FogFragCoord = FogSrc;\\n\":fogdist==FPE_FOG_DIST_RADIAL?\"gl_FogFragCoord = length(FogSrc);\\n\":\"gl_FogFragCoord = abs(FogSrc);\\n\");\n        #endif")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/stack.h"
  "    GLboolean alpha_test;\n    GLint alpha_test_func;"
  "    GLboolean alpha_test;\n    GLboolean vertex_program_arb, fragment_program_arb;\n    GLint alpha_test_func;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/stack.c"
  "        cur->alpha_test = gl4es_glIsEnabled(GL_ALPHA_TEST);\n        cur->autonormal"
  "        cur->alpha_test = gl4es_glIsEnabled(GL_ALPHA_TEST);\n        cur->vertex_program_arb = glstate->enable.vertex_arb;\n        cur->fragment_program_arb = glstate->enable.fragment_arb;\n        cur->autonormal")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/stack.c"
  "        enable_disable(GL_ALPHA_TEST, cur->alpha_test);\n        enable_disable(GL_AUTO_NORMAL"
  "        enable_disable(GL_ALPHA_TEST, cur->alpha_test);\n        enable_disable(GL_VERTEX_PROGRAM_ARB, cur->vertex_program_arb);\n        enable_disable(GL_FRAGMENT_PROGRAM_ARB, cur->fragment_program_arb);\n        enable_disable(GL_AUTO_NORMAL")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/enable.c"
  "        isenabled(GL_VERTEX_PROGRAM_TWO_SIDE_ARB, vertex_two_side_arb);\n        default:"
  "        isenabled(GL_VERTEX_PROGRAM_TWO_SIDE_ARB, vertex_two_side_arb);\n        isenabled(GL_FRAGMENT_PROGRAM_ARB, fragment_arb);\n        default:")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe.c"
  "        gl4es_glShaderSource(glstate->fpe->vert, 1, fpe_CustomVertexShader(old_vtx->shader->source, state, state->fragment_prg_id?0:1), NULL);\n        gl4es_glCompileShader(glstate->fpe->vert);"
  "        // an ARB fragment program reading the secondary colour, drawn with a\n        // vertex program that does not write it: write 0 (OpenGL ES refuses\n        // to link a fragment input no vertex output feeds)\n        const char *vsrc = old_vtx->shader->source;\n        char *vsec = NULL;\n        if(state->fragment_prg_id && old_frg && strstr(old_frg->shader->source, \"gl_SecondaryColor\") && !strstr(vsrc, \"gl_FrontSecondaryColor\")) {\n            const char *m = strstr(vsrc, \"main()\");\n            const char *br = m ? strchr(m, '{') : NULL;\n            if(br) {\n                const char *add = \"\\ngl_FrontSecondaryColor = vec4(0.0);\\n\";\n                vsec = (char*)malloc(strlen(vsrc)+strlen(add)+1);\n                memcpy(vsec, vsrc, br-vsrc+1);\n                strcpy(vsec+(br-vsrc+1), add);\n                strcat(vsec, br+1);\n                vsrc = vsec;\n            }\n        }\n        gl4es_glShaderSource(glstate->fpe->vert, 1, fpe_CustomVertexShader(vsrc, state, state->fragment_prg_id?0:1), NULL);\n        free(vsec);\n        gl4es_glCompileShader(glstate->fpe->vert);")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/gl4es.c"
  "    t.normalized=n; t.divisor=0\n#define break_lockarrays"
  "    t.normalized=n; t.divisor=0; t.buffer=NULL\n#define break_lockarrays")
