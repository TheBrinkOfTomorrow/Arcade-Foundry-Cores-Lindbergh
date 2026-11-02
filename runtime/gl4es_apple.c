// gl4es's second names that are aliases elsewhere (see gl4es_apple.h)
#include <GL/gl.h>

void gl4es_glEnableClientStateIndexed(GLenum array, GLuint index);
void gl4es_glDisableClientStateIndexed(GLenum array, GLuint index);

void gl4es_glEnableClientStatei(GLenum array, GLuint index) { gl4es_glEnableClientStateIndexed(array, index); }
void gl4es_glDisableClientStatei(GLenum array, GLuint index) { gl4es_glDisableClientStateIndexed(array, index); }
