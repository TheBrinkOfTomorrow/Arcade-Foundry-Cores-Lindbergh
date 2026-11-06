# Fixes to gl4es (MIT) at the pinned commit, applied to the fetched source
# at configure time (each once; see gl4es_fix below):
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
#  - fpe.c: a uniform's number in its name (_gl4es_Vertex_ProgramLocal_188)
#    was read from its first two digits, so ARB program parameters from 100
#    up (local and env, vertex and fragment) were set in the wrong slots (188
#    in 18): Let's Go Jungle's 3D, whose matrices are program.local[7..233],
#    was drawn with the wrong ones. All its digits are read now.
#  - hardext.c, texture.c: depth textures are core on OpenGL ES 3 (not
#    listed as an extension), and GL_DEPTH_COMPONENT24 was missing from the
#    internal formats, so Let's Go Jungle's depth targets (its shadow map
#    among them) were made RGBA and drawn with a 16-bit depth renderbuffer.
#    (host_core_ios.m gives OpenGL ES 3 the sized depth formats it needs.)
#  - shaderconv.c: a built-in attribute was looked for as any
#    substring, so _gl4es_Vertex was found in _gl4es_VertexAttrib_0: an ARB
#    vertex program reading vertex.attrib[0] got an unused _gl4es_Vertex
#    bound to location 0 and its real position input put wherever the linker
#    chose, and every vertex was read from another array (Let's Go Jungle's
#    3D, which reads all its inputs as generic attributes). Whole names only;
#    and generic attributes 1, 6 and 7, which have no built-in name, were
#    never bound at all (Let's Go Jungle's bone weights and indices: its
#    skinned models were drawn as spikes).
#  - arbgenerator.c: a parameter array read at a run-time index outside it
#    (ARB leaves it undefined) read whatever Apple's driver returned there,
#    NaN at times; the index is clamped to the array.
#  - arbgenerator.c: SIN and COS were refused in vertex programs (plain ARB
#    has them for fragment programs only; NVIDIA's vp40, which the iOS build
#    takes, has them: Let's Go Jungle's swaying plants), and SIN's "sin("
#    was appended as 9 characters, 5 of them whatever followed it in memory.
#  - queries.c, state.h: occlusion queries counted nothing (every query found
#    no samples passed): OutRun 2 SP sizes its sun glare by the pixels of the
#    sun a query finds visible, so the glare and the scene's white-out never
#    showed. GL_SAMPLES_PASSED and GL_ANY_SAMPLES_PASSED(_CONSERVATIVE) now go
#    to the driver's queries when it has them (OpenGL ES 3, or
#    EXT_occlusion_query_boolean); a driver that refuses GL_SAMPLES_PASSED
#    (OpenGL ES has the boolean ones only) gets GL_ANY_SAMPLES_PASSED.
#  - shaderconv.c: ConvertShader's output is kept, by its source, stage and
#    the needs it was given, and a conversion already made is a copy (issue
#    #63). gl4es converts an ARB program pair's shaders again for each new
#    fixed-function state it is drawn with (fpe_oldprogram, redoShader at
#    link): Sega Race TV makes 216 programs from 28 vertex and 28 fragment
#    programs while it loads, each conversion mostly string passes.
#  - texture_read.c: glCopyTexSubImage2D into a depth texture went through
#    glReadPixels(GL_DEPTH_COMPONENT, GL_UNSIGNED_INT), which gl4es reads as
#    colour and mtlgles does not do, so the texture never held depth (HotD4
#    copies its scene's depth each frame for its depth of field). A depth
#    texture is now a depth framebuffer blit (OpenGL ES 3 copies no depth
#    with glCopyTexSubImage2D).
#    On OpenGL ES 3 a depth blit needs the same format at both ends: the
#    window's depth is packed with its stencil (DEPTH24_STENCIL8) and the
#    game's texture is depth only, so Apple's driver refused every copy
#    (GL_INVALID_OPERATION, HotD4 on the iPad). A depth texture copied from a
#    packed depth+stencil attachment is given packed storage before its
#    first copy (texture.h: depth_packed, which glTexImage2D of level 0
#    clears and a deleted texture's name doesn't keep).
#  - hardext.c, hardext.h, texture.h, texture_compressed.c: DXT1/3/5 were
#    always decoded to RGBA (or 16-bit) on the CPU. A driver that offers
#    GL_EXT_texture_compression_s3tc (mtlgles where the device samples BC)
#    now gets them compressed: OutRun 2 SP, HotD4 and Race TV's textures took
#    184, 348 and 442 MB decoded, 58, 120 and 166 MB kept compressed, and
#    Race TV's boot decoded for 1.7 s. sRGB DXT, generated mipmaps and a
#    texture decoded at another level still decode; LIBGL_NODXTPASS=1 always
#    decodes.
#  - queries.c, state.h: a GL_SAMPLES_PASSED query answered by such a boolean
#    query reported 1 for "some passed", which a game reads as next to
#    nothing (OutRun 2 SP divides the count by 12000 for its glare's alpha:
#    no glare on the iPad). A true result now reports the viewport's pixels
#    (taken at glBeginQuery), the most there can be.
#  - texture_params.c: glBindTexture on a cube-map target binds on the driver
#    at once (2D binds wait for the draw), but on the driver's active unit as
#    last realized, not the one glActiveTexture chose since: OutRun 2 SP's
#    sky cube for unit 3 landed on another unit, and its car's reflection
#    sampled the wrong cube map or none. The active unit is realized first.
#  - texture_params.c: a texture's filters and wraps reach the driver only in
#    realize_textures, for the target enabled on the unit; a cube map an ARB
#    fragment program samples is never enabled, so the driver kept its
#    default mipmapped min filter. OutRun 2 SP renders only level 0 of its
#    reflection cube: the car's reflection read levels never drawn (magenta
#    on mtlgles). Each bound cube map's sampler state is brought up to date
#    at every draw.
#  - texture.c, framebuffers.c: OutRun 2 SP makes its render targets' depth
#    textures with the unsized GL_DEPTH_STENCIL, which gl4es's internal
#    formats did not list (only GL_DEPTH24_STENCIL8): the texture was taken
#    for RGBA, and its first depth attachment remade it depth only. The
#    stencil attachment that follows (the same texture, as
#    GL_DEPTH_STENCIL_ATTACHMENT: hle_gfx.c) was then taken as already done,
#    since gl4es read a depth+stencil attachment as its depth one. The race's
#    framebuffer had no stencil, and the car's shadow volume pass (stencil
#    test "more than 0") darkened whatever the GPU left there: an 800x480
#    rectangle at the top left, on mtlgles and on the iPad (#78/#79's band
#    at x = 800). Now made GL_DEPTH_STENCIL, and attached as stencil too.
#  - texture_compressed.c: glGetCompressedTexImage worked only for a texture
#    the game uploaded uncompressed with GL_COMPRESSED_RGB(A) (decoded, then
#    compressed again on the CPU). A DXT texture kept compressed on the driver
#    (above) was refused: Sega Race TV reads its cars' DXT textures back at
#    boot and recolours them, and on mtlgles the cars were painted with
#    whatever its buffer held (noise, the wrong colours, cars nearly black).
#    Such a texture's level is now read from the driver block for block (the
#    driver's glGetCompressedTexImage: mtlgles has it), exactly as uploaded.
# Each fix is applied once, to the pristine gl4es, and an entry whose "from" is not
# there stops the configure (most likely an earlier entry changed that text: replay
# them with tools/replayfix.py). The patched source is then stamped with this file's
# SHA-256, and a configure finding that stamp applies nothing; CMakeLists.txt has a
# source stamped by another version of this file extracted again.
if(EXISTS "${gl4es_SOURCE_DIR}/.gl4es_fixes")
  file(READ "${gl4es_SOURCE_DIR}/.gl4es_fixes" stamp)
  if(stamp STREQUAL GL4ES_FIXES_SHA)
    return()
  endif()
  message(FATAL_ERROR "gl4es_fixes.cmake: ${gl4es_SOURCE_DIR} was patched by another "
    "version of this file; give FETCHCONTENT_SOURCE_DIR_GL4ES a pristine gl4es")
endif()
set(gl4es_fix_n 0)
function(gl4es_fix file from to)
  math(EXPR n "${gl4es_fix_n} + 1")
  set(gl4es_fix_n ${n} PARENT_SCOPE)
  file(READ "${file}" text)
  string(FIND "${text}" "${from}" at)
  if(at EQUAL -1)
    file(RELATIVE_PATH rel "${gl4es_SOURCE_DIR}" "${file}")
    set(why "an earlier entry may have changed it: tools/replayfix.py")
    if(FETCHCONTENT_SOURCE_DIR_GL4ES)
      set(why "${why}; FETCHCONTENT_SOURCE_DIR_GL4ES must be a pristine gl4es")
    endif()
    message(FATAL_ERROR "gl4es_fixes.cmake: entry ${n} (${rel}) matches nothing (${why}). "
      "Not in the source:\n${from}")
  endif()
  string(REPLACE "${from}" "${to}" text "${text}")
  file(WRITE "${file}" "${text}")
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
  "    S(\"GL_OES_rgb8_rgba8 \", rgba8, 1);\n    {\n        const char *ver = (const char *)gles_glGetString(GL_VERSION);\n        if (ver && strstr(ver, \"OpenGL ES 3\")) {\n            hardext.depthstencil = hardext.depth24 = hardext.rgba8 = hardext.elementuint = 1;\n            if(!globals4es.nodepthtex) hardext.depthtex = 1;\n            SHUT_LOGD(\"OpenGL ES 3: depth textures, packed depth stencil, depth24, rgba8 and uint indices are core\\n\");\n        }\n    }\n")
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
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe.c"
  "        int n = name[l]-'0';\n        if(name[l+1]>='0' && name[l+1]<='9')\n            n = n*10 + name[l+1]-'0';\n"
  "        int n = 0;\n        for (const char *d = name + l; *d >= '0' && *d <= '9'; ++d)\n            n = n*10 + *d - '0';\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/fpe.c"
  "                int n = name[l]-'0';\n                if(name[l+1]>='0' && name[l+1]<='9')\n                    n = n*10 + name[l+1]-'0';\n"
  "                int n = 0;\n                for (const char *d = name + l; *d >= '0' && *d <= '9'; ++d)\n                    n = n*10 + *d - '0';\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/shaderconv.c"
  "          if(strstr(Tmp, builtin_attrib[i].glname)) {"
  "          if(gl4es_find_string(Tmp, builtin_attrib[i].glname)) {")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/shaderconv.c"
  "    if (!ret)\n      return NULL;\n    if(strstr(vertexShader, ret)) // it's here!\n      return ret;\n    // check for old program generic vertex attribs\n    if(strstr(vertexShader, gl4es_VA[Att]))"
  "    if(ret && gl4es_find_string(vertexShader, ret)) // it's here!\n      return ret;\n    // check for old program generic vertex attribs\n    if(gl4es_find_string(vertexShader, gl4es_VA[Att]))")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture.c"
  "        case GL_DEPTH_COMPONENT16:\n            if(hardext.depthtex) {"
  "        case GL_DEPTH_COMPONENT16:\n        case GL_DEPTH_COMPONENT24:\n            if(hardext.depthtex) {")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/arbgenerator.c"
  "#define PUSH_VARNAME(i) \\\n\t\tif (instPtr->vars[i].sign == -1) {                        \\\n\t\t\tAPPEND_OUTPUT(\"-\", 1)                                 \\\n\t\t}                                                         \\\n\t\tif (instPtr->vars[i].var->type == VARTYPE_CONST) {        \\\n\t\t\tAPPEND_OUTPUT2(instPtr->vars[i].var->init.strings[0]) \\\n\t\t} else {                                                  \\\n\t\t\tAPPEND_OUTPUT2(instPtr->vars[i].var->names[0])        \\\n\t\t}                                                         \\\n\t\tif (instPtr->vars[i].floatArrAddr != NULL) {              \\\n\t\t\tAPPEND_OUTPUT(\"[\", 1)                                 \\\n\t\t\tAPPEND_OUTPUT2(instPtr->vars[i].floatArrAddr)         \\\n\t\t\tAPPEND_OUTPUT(\"]\", 1)                                 \\\n\t\t}\n"
  "#define PUSH_VARNAME(i) \\\n\t\tif (instPtr->vars[i].sign == -1) {                        \\\n\t\t\tAPPEND_OUTPUT(\"-\", 1)                                 \\\n\t\t}                                                         \\\n\t\tif (instPtr->vars[i].var->type == VARTYPE_CONST) {        \\\n\t\t\tAPPEND_OUTPUT2(instPtr->vars[i].var->init.strings[0]) \\\n\t\t} else {                                                  \\\n\t\t\tAPPEND_OUTPUT2(instPtr->vars[i].var->names[0])        \\\n\t\t}                                                         \\\n\t\tif (instPtr->vars[i].floatArrAddr != NULL) {              \\\n\t\t\tchar lbN[24];                                         \\\n\t\t\tsnprintf(lbN, sizeof lbN, \"), 0.0, %d.0))]\", instPtr->vars[i].var->size - 1); \\\n\t\t\tAPPEND_OUTPUT(\"[int(clamp(float(\", 17)                \\\n\t\t\tAPPEND_OUTPUT2(instPtr->vars[i].floatArrAddr)         \\\n\t\t\tAPPEND_OUTPUT2(lbN)                                   \\\n\t\t}\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/arbgenerator.c"
  "\tcase INST_COS:\n\t\tif (vertex) {\n\t\t\tFAIL(\"Invalid instruction in vertex shader\");\n\t\t}\n"
  "\tcase INST_COS:   // in vertex programs too (NVIDIA's vp40)\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/arbgenerator.c"
  "\tcase INST_SIN:\n\t\tif (vertex) {\n\t\t\tFAIL(\"Invalid instruction in vertex shader\");\n\t\t}\n"
  "\tcase INST_SIN:   // in vertex programs too (NVIDIA's vp40)\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/arbgenerator.c"
  "\t\tAPPEND_OUTPUT(\"sin(\", 9)"
  "\t\tAPPEND_OUTPUT(\"sin(\", 4)")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/state.h"
  "    GLuint start;\n} glquery_t;"
  "    GLuint start;\n    GLuint hw;         // the driver's own query (occlusion queries)\n    GLenum hwtarget;\n} glquery_t;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "    return NULL;\n}\n\n\nvoid del_querie(GLuint querie) {"
  "    return NULL;\n}\n\n// occlusion queries on the driver (OpenGL ES 3, or EXT_occlusion_query_boolean):\n// gl4es counted no samples, every query found none\ntypedef void (*hwq_gen_fn)(GLsizei, GLuint *);\ntypedef void (*hwq_begin_fn)(GLenum, GLuint);\ntypedef void (*hwq_end_fn)(GLenum);\ntypedef void (*hwq_get_fn)(GLuint, GLenum, GLuint *);\ntypedef GLenum (*hwq_err_fn)(void);\ntypedef const GLubyte *(*hwq_str_fn)(GLenum);\nstatic struct {\n    int tried, samples, checked;\n    hwq_gen_fn gen, del;\n    hwq_begin_fn begin;\n    hwq_end_fn end;\n    hwq_get_fn get;\n    hwq_err_fn err;\n} hwq;\nstatic int hw_queries(GLenum target) {\n    if(target!=GL_SAMPLES_PASSED && target!=GL_ANY_SAMPLES_PASSED && target!=GL_ANY_SAMPLES_PASSED_CONSERVATIVE)\n        return 0;\n    if(!hwq.tried) {\n        hwq.tried = 1;\n        hwq_str_fn str = (hwq_str_fn)proc_address(gles, \"glGetString\");\n        const char *ver = str ? (const char *)str(GL_VERSION) : NULL;\n        const char *ext = str ? (const char *)str(GL_EXTENSIONS) : NULL;\n        const char *sfx = (ver && strstr(ver, \"OpenGL ES 3\")) ? \"\" : (ext && strstr(ext, \"GL_EXT_occlusion_query_boolean\")) ? \"EXT\" : NULL;\n        if(sfx) {\n            char n[64];\n            snprintf(n, sizeof n, \"glGenQueries%s\", sfx); hwq.gen = (hwq_gen_fn)proc_address(gles, n);\n            snprintf(n, sizeof n, \"glDeleteQueries%s\", sfx); hwq.del = (hwq_gen_fn)proc_address(gles, n);\n            snprintf(n, sizeof n, \"glBeginQuery%s\", sfx); hwq.begin = (hwq_begin_fn)proc_address(gles, n);\n            snprintf(n, sizeof n, \"glEndQuery%s\", sfx); hwq.end = (hwq_end_fn)proc_address(gles, n);\n            snprintf(n, sizeof n, \"glGetQueryObjectuiv%s\", sfx); hwq.get = (hwq_get_fn)proc_address(gles, n);\n            hwq.err = (hwq_err_fn)proc_address(gles, \"glGetError\");\n            if(!hwq.del || !hwq.begin || !hwq.end || !hwq.get || !hwq.err)\n                hwq.gen = NULL;\n            hwq.samples = 1;   // sample counts, until the driver refuses GL_SAMPLES_PASSED\n        }\n    }\n    return hwq.gen!=NULL;\n}\n// a query's result from the driver; 0: not a driver's query\nstatic int hw_result(glquery_t *query, GLenum pname, GLuint *v) {\n    if(!query->hw || !query->hwtarget || (pname!=GL_QUERY_RESULT && pname!=GL_QUERY_RESULT_AVAILABLE && pname!=GL_QUERY_RESULT_NO_WAIT))\n        return 0;\n    if(pname==GL_QUERY_RESULT_NO_WAIT) {\n        GLuint ready = 0;\n        hwq.get(query->hw, GL_QUERY_RESULT_AVAILABLE, &ready);\n        if(ready)\n            hwq.get(query->hw, GL_QUERY_RESULT, v);\n    } else\n        hwq.get(query->hw, pname, v);\n    return 1;\n}\n\n\nvoid del_querie(GLuint querie) {")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "    if(s)\n        free(s);\n}"
  "    if(s && s->hw && hwq.del)\n        hwq.del(1, &s->hw);\n    if(s)\n        free(s);\n}")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "\tquery->active = 1;\n\tquery->start = get_clock() - glstate->queries.start;\n    noerrorShim();\n}"
  "\tquery->active = 1;\n\tquery->start = get_clock() - glstate->queries.start;\n    query->hwtarget = 0;\n    if(hw_queries(target)) {\n        if(!query->hw)\n            hwq.gen(1, &query->hw);\n        GLenum t = (target==GL_SAMPLES_PASSED && !hwq.samples) ? GL_ANY_SAMPLES_PASSED : target;\n        hwq.begin(t, query->hw);\n        if(t==GL_SAMPLES_PASSED && !hwq.checked) {   // OpenGL ES has the boolean ones only\n            hwq.checked = 1;\n            if(hwq.err()==GL_INVALID_ENUM) {\n                hwq.samples = 0;\n                t = GL_ANY_SAMPLES_PASSED;\n                hwq.begin(t, query->hw);\n            }\n        }\n        query->hwtarget = t;\n    }\n    noerrorShim();\n}")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "    query->active = 0;\n\tquery->start = (get_clock() - glstate->queries.start) - query->start;"
  "    query->active = 0;\n    if(query->hw && query->hwtarget)\n        hwq.end(query->hwtarget);\n\tquery->start = (get_clock() - glstate->queries.start) - query->start;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "void APIENTRY_GL4ES gl4es_glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {\n    FLUSH_BEGINEND;\n\n\tglquery_t *query = find_query(id);\n\tif(!query) {\n\t\terrorShim(GL_INVALID_OPERATION);\n\t\treturn;\n\t}\n"
  "void APIENTRY_GL4ES gl4es_glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {\n    FLUSH_BEGINEND;\n\n\tglquery_t *query = find_query(id);\n\tif(!query) {\n\t\terrorShim(GL_INVALID_OPERATION);\n\t\treturn;\n\t}\n    GLuint hv;\n    if(hw_result(query, pname, &hv)) {\n        if(pname!=GL_QUERY_RESULT_NO_WAIT || hv) *params = (GLint)hv;\n        noerrorShim();\n        return;\n    }\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "void APIENTRY_GL4ES gl4es_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {\n    FLUSH_BEGINEND;\n\t\t\n\tglquery_t *query = find_query(id);\n\tif(!query) {\n\t\terrorShim(GL_INVALID_OPERATION);\n\t\treturn;\n\t}\n"
  "void APIENTRY_GL4ES gl4es_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {\n    FLUSH_BEGINEND;\n\t\t\n\tglquery_t *query = find_query(id);\n\tif(!query) {\n\t\terrorShim(GL_INVALID_OPERATION);\n\t\treturn;\n\t}\n    if(hw_result(query, pname, params)) {\n        noerrorShim();\n        return;\n    }\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/shaderconv.c"
  [=[char* ConvertShader(const char* pEntry, int isVertex, shaderconv_need_t *need)
{]=]
  [=[static char* ConvertShader_conv(const char* pEntry, int isVertex, shaderconv_need_t *need);
// conversions made, by source, stage and the needs given (and the needs they left)
typedef struct { uint64_t hash; int vertex, has_need; shaderconv_need_t in, out; char *source, *converted; } convcache_t;
static convcache_t *convcache;
static int convcache_n, convcache_cap;
char* ConvertShader(const char* pEntry, int isVertex, shaderconv_need_t *need)
{
  uint64_t hash = 1469598103934665603ULL;
  for(const unsigned char *p = (const unsigned char *)pEntry; *p; ++p)
    hash = (hash ^ *p) * 1099511628211ULL;
  shaderconv_need_t in;
  memset(&in, 0, sizeof(in));
  if(need) in = *need;
  for(int i=0; i<convcache_n; ++i) {
    convcache_t *e = &convcache[i];
    if(e->hash==hash && e->vertex==isVertex && e->has_need==(need!=NULL) && (!need || !memcmp(&e->in, &in, sizeof(in))) && !strcmp(e->source, pEntry)) {
      if(need) *need = e->out;
      return strdup(e->converted);
    }
  }
  char *converted = ConvertShader_conv(pEntry, isVertex, need);
  if(converted && convcache_n < 4096) {
    if(convcache_n == convcache_cap) {
      convcache_cap = convcache_cap ? convcache_cap * 2 : 64;
      convcache = (convcache_t*)realloc(convcache, convcache_cap * sizeof(convcache_t));
    }
    convcache_t *e = &convcache[convcache_n++];
    e->hash = hash, e->vertex = isVertex, e->has_need = (need!=NULL);
    e->in = in;
    memset(&e->out, 0, sizeof(e->out));
    if(need) e->out = *need;
    e->source = strdup(pEntry);
    e->converted = strdup(converted);
  }
  return converted;
}
static char* ConvertShader_conv(const char* pEntry, int isVertex, shaderconv_need_t *need)
{]=])
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_read.c"
  "    if(!blit || target!=GL_TEXTURE_2D || !bound || !bound->glname || bound->format!=GL_RGBA || bound->type!=GL_UNSIGNED_BYTE)\n        return 0;"
  "    int depth = bound && bound->format==GL_DEPTH_COMPONENT;\n    if(!blit || target!=GL_TEXTURE_2D || !bound || !bound->glname || (!depth && (bound->format!=GL_RGBA || bound->type!=GL_UNSIGNED_BYTE)))\n        return 0;\n    GLenum att = depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_read.c"
  "    fbtex(0x8CA9, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bound->glname, level);\n    blit(x, y, x+width, y+height, xoffset, yoffset, xoffset+width, yoffset+height, GL_COLOR_BUFFER_BIT, GL_NEAREST);\n    fbtex(0x8CA9, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);"
  "    fbtex(0x8CA9, att, GL_TEXTURE_2D, bound->glname, level);\n    blit(x, y, x+width, y+height, xoffset, yoffset, xoffset+width, yoffset+height, depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);\n    fbtex(0x8CA9, att, GL_TEXTURE_2D, 0, 0);")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_read.c"
  "        copytex = ((bound->format==GL_RGBA && bound->type==GL_UNSIGNED_BYTE) "
  "        copytex = bound->format==GL_DEPTH_COMPONENT || ((bound->format==GL_RGBA && bound->type==GL_UNSIGNED_BYTE) ")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_read.c"
  "    bindfb(0x8CA9, fb);    // GL_DRAW_FRAMEBUFFER\n    fbtex(0x8CA9, att,"
  "    bindfb(0x8CA9, fb);    // GL_DRAW_FRAMEBUFFER\n    if(depth) {   // a packed depth+stencil source: the texture packed too (once)\n        static void (*attr)(GLenum, GLenum, GLenum, GLint *);\n        static void (*teximage)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);\n        static void (*bindtex)(GLenum, GLuint);\n        if(!attr) {\n            attr = proc_address(gles, \"glGetFramebufferAttachmentParameteriv\");\n            teximage = proc_address(gles, \"glTexImage2D\");\n            bindtex = proc_address(gles, \"glBindTexture\");\n        }\n        GLint dname = 0, sname = 0;\n        if(cur && attr && teximage && bindtex) {\n            attr(0x8CA8, GL_DEPTH_ATTACHMENT, 0x8CD1, &dname);     // GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME\n            attr(0x8CA8, 0x8D20, 0x8CD1, &sname);                  // GL_STENCIL_ATTACHMENT\n        }\n        if(dname && dname==sname && !bound->depth_packed) {\n            GLint was = 0;\n            geti(0x8069, &was);   // GL_TEXTURE_BINDING_2D\n            bindtex(GL_TEXTURE_2D, bound->glname);\n            teximage(GL_TEXTURE_2D, level, 0x88F0, bound->nwidth, bound->nheight, 0, 0x84F9, 0x84FA, NULL);   // DEPTH24_STENCIL8\n            bindtex(GL_TEXTURE_2D, was);\n            bound->depth_packed = 1;\n        }\n    }\n    fbtex(0x8CA9, att,")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture.h"
  "    int     trace;  // internal use for debug\n"
  "    int     trace;  // internal use for debug\n    int     depth_packed;   // given packed depth+stencil storage for depth copies (texture_read.c)\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture.c"
  "        bound->wanted_internal = internalformat;    // save it before transformation\n"
  "        bound->wanted_internal = internalformat;    // save it before transformation\n        bound->depth_packed = 0;   // a new image: the depth copy repacks it again if it needs to\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/glx/hardext.h"
  "    int rgba8;          // GL_OES_rgb8_rgba8\n"
  "    int rgba8;          // GL_OES_rgb8_rgba8\n    int s3tc;           // GL_EXT_texture_compression_s3tc: DXT1/3/5 kept compressed\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/glx/hardext.c"
  "    S(\"GL_OES_depth24 \", depth24, 1);\n"
  "    S(\"GL_OES_depth24 \", depth24, 1);\n    {   // the whole word (it may end the list)\n        const char *s3 = strstr(Exts, \"GL_EXT_texture_compression_s3tc\");\n        if (s3 && (s3[31]==' ' || s3[31]==0) && !getenv(\"LIBGL_NODXTPASS\")) {\n            hardext.s3tc = 1;\n            SHUT_LOGD(\"Extension GL_EXT_texture_compression_s3tc detected and used\\n\");\n        }\n    }\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture.h"
  "    GLboolean compressed;\n"
  "    GLboolean compressed;\n    GLboolean native_dxt;   // DXT kept compressed on the driver (hardext.s3tc)\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_compressed.c"
  "    GLenum format = GL_RGBA;\n    GLenum type = GL_UNSIGNED_BYTE;\n        \n    if (isDXTc(internalformat)) {\n"
  "    GLenum format = GL_RGBA;\n    GLenum type = GL_UNSIGNED_BYTE;\n        \n    // the driver's own S3TC: the texture stays compressed (a quarter to an eighth of RGBA8, no\n    // decoding); not for sRGB, generated mipmaps, or a texture already decoded at another level\n    if (isDXTc(internalformat) && hardext.s3tc && !isDXTcSRGB(internalformat) && !bound->mipmap_auto\n        && (!level || !bound->valid || bound->native_dxt)) {\n        LOAD_GLES(glCompressedTexImage2D);\n        int rgb = internalformat==GL_COMPRESSED_RGB_S3TC_DXT1_EXT;\n        bound->alpha = rgb?0:1;\n        bound->format = rgb?GL_RGB:GL_RGBA;\n        bound->type = GL_UNSIGNED_BYTE;\n        bound->wanted_internal = bound->internalformat = internalformat;\n        bound->fpe_format = rgb?FPE_TEX_RGB:FPE_TEX_RGBA;\n        bound->compressed = 1;\n        bound->native_dxt = 1;\n        bound->valid = 1;\n        if (!level) {\n            bound->width = bound->nwidth = width;\n            bound->height = bound->nheight = height;\n            bound->npot = bound->adjust = 0;\n        }\n        if (glstate->fpe_state && glstate->fpe_bound_changed < glstate->texture.active+1)\n            glstate->fpe_bound_changed = glstate->texture.active+1;\n        gles_glCompressedTexImage2D(rtarget, level, internalformat, width, height, border, imageSize, datab);\n        errorGL();\n        glstate->vao->unpack = unpack;\n        return;\n    }\n    if (isDXTc(internalformat)) {\n        bound->native_dxt = 0;\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_compressed.c"
  "    if (isDXTc(format)) {\n"
  "    if (isDXTc(format) && bound->native_dxt) {   // kept compressed: so is its update\n        gles_glCompressedTexSubImage2D(map_tex_target(target), level, xoffset, yoffset, width, height, format, imageSize, datab);\n        errorGL();\n    } else if (isDXTc(format)) {\n")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/state.h"
  "    GLuint hw;         // the driver's own query (occlusion queries)\n    GLenum hwtarget;\n} glquery_t;"
  "    GLuint hw;         // the driver's own query (occlusion queries)\n    GLenum hwtarget;\n    GLuint area;       // GL_SAMPLES_PASSED on a boolean query: a true result's count\n} glquery_t;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "    } else\n        hwq.get(query->hw, pname, v);\n    return 1;\n}"
  "    } else\n        hwq.get(query->hw, pname, v);\n    // GL_SAMPLES_PASSED answered by a boolean query: samples passed reports the\n    // viewport's pixels, the most there can be (1 would read as next to none)\n    if(query->area && pname!=GL_QUERY_RESULT_AVAILABLE && *v)\n        *v = query->area;\n    return 1;\n}")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/queries.c"
  "        query->hwtarget = t;\n    }\n    noerrorShim();"
  "        query->hwtarget = t;\n        query->area = (target==GL_SAMPLES_PASSED && t!=GL_SAMPLES_PASSED) ? (GLuint)(glstate->raster.viewport.width*glstate->raster.viewport.height) : 0;\n    }\n    noerrorShim();")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_params.c"
  "            case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:\n                gles_glBindTexture(target, tex?tex->glname:0);"
  "            case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:\n                realize_active();   // on the unit the game chose, not the one last realized\n                gles_glBindTexture(target, tex?tex->glname:0);")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_params.c"
  "        realize_1texture(target, i, tex, glstate->samplers.sampler[i]);\n    }\n    glstate->bound_changed = 0;\n}"
  "        realize_1texture(target, i, tex, glstate->samplers.sampler[i]);\n    }\n    glstate->bound_changed = 0;\n    // cube maps are bound at once but their filters and wraps reach the driver\n    // only here: also the ones an ARB fragment program samples without\n    // GL_TEXTURE_CUBE_MAP enabled on the unit\n    if(drawing)\n        for (int i=0; i<hardext.maxtex; i++) {\n            gltexture_t *cube = glstate->texture.bound[i][ENABLED_CUBE_MAP];\n            if(cube && cube->glname && cube->valid)\n                realize_1texture(GL_TEXTURE_CUBE_MAP, i, cube, glstate->samplers.sampler[i]);\n        }\n}")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture.c"
  "        case GL_DEPTH24_STENCIL8:\n            if(hardext.depthtex) {\n                sret = ret = GL_DEPTH_STENCIL;"
  "        case GL_DEPTH_STENCIL:\n        case GL_DEPTH24_STENCIL8:\n            if(hardext.depthtex) {\n                sret = ret = GL_DEPTH_STENCIL;")
gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/framebuffers.c"
  "    case GL_DEPTH_STENCIL_ATTACHMENT:\n        // is that possible?\n        return fb->depth;"
  "    case GL_DEPTH_STENCIL_ATTACHMENT:\n        // attached as both only (a texture attached as depth alone is not)\n        return fb->depth==fb->stencil ? fb->depth : 0;")

gl4es_fix("${gl4es_SOURCE_DIR}/src/gl/texture_compressed.c"
  "    errorShim(GL_INVALID_OPERATION);\n    if(bound->orig_internal!=GL_COMPRESSED_RGB && bound->orig_internal!=GL_COMPRESSED_RGBA)\n        return;"
  "    errorShim(GL_INVALID_OPERATION);\n    if(bound->native_dxt) {   // kept compressed on the driver: its blocks, as they are\n        static void (APIENTRY_GL4ES *get)(GLenum, GLint, GLvoid*) = NULL;\n        static int tried = 0;\n        if(!tried) { tried = 1; get = proc_address(gles, \"glGetCompressedTexImage\"); }\n        if(!get) return;\n        realize_bound(glstate->texture.active, target);\n        GLvoid *datab = img;\n        if(glstate->vao->pack)\n            datab = (char*)datab + (uintptr_t)glstate->vao->pack->data;\n        get(map_tex_target(target), lod, datab);\n        noerrorShim();\n        return;\n    }\n    if(bound->orig_internal!=GL_COMPRESSED_RGB && bound->orig_internal!=GL_COMPRESSED_RGBA)\n        return;")

file(WRITE "${gl4es_SOURCE_DIR}/.gl4es_fixes" "${GL4ES_FIXES_SHA}")
