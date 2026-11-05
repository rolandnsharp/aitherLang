/* aither export: a patch as C to build into any program (SPEC.md,
 * "Export"). */
#ifndef AITHER_EXPORT_H
#define AITHER_EXPORT_H

/* Write <dir>/<name>.c and <name>.h (the patch and its interface) and
 * <dir>/aither_rt.c and aither_rt.h (the runtime), the patch compiled for
 * `rate`. 0, or -1 with the message in err. */
int  export_patch(const char* path, const char* dir, const char* name, double rate, char* err, int errlen);
/* The C name a patch file exports as: its base name, made an identifier. */
void export_name_of(const char* path, char* out, int outlen);

#endif
