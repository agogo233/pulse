/* Core miniz helpers (mz_free, default allocators, version) from the miniz
   release amalgamation; the split master layout keeps them in miniz.c, which
   this vendored copy does not use. Unlicense; see NOTICE.txt. */
#include "miniz.h"

void mz_free(void *p)
{
    MZ_FREE(p);
}

MINIZ_EXPORT void *miniz_def_alloc_func(void *opaque, size_t items, size_t size)
{
    (void)opaque, (void)items, (void)size;
    return MZ_MALLOC(items * size);
}
MINIZ_EXPORT void miniz_def_free_func(void *opaque, void *address)
{
    (void)opaque, (void)address;
    MZ_FREE(address);
}
MINIZ_EXPORT void *miniz_def_realloc_func(void *opaque, void *address, size_t items, size_t size)
{
    (void)opaque, (void)address, (void)items, (void)size;
    return MZ_REALLOC(address, items * size);
}

const char *mz_version(void)
{
    return MZ_VERSION;
}
