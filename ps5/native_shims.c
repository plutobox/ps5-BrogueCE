/* native_shims.c - C library functions for the native PS5 title that
 * neither the console nor mihawk-99's platform layer provide.
 *
 * package-native.sh binds libc's names to these brogue_ps5_ functions with
 * --defsym, as it binds the platform layer's ps5_ functions: a title must
 * not define libc's own names. */

#include <sys/stat.h>

/* lstat() fails for a title. A title has no symbolic links, so stat()
 * answers the same. */
int brogue_ps5_lstat(const char *restrict path, struct stat *restrict sb)
{
    return stat(path, sb);
}
