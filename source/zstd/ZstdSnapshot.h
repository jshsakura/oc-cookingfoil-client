#ifndef COOKINGFOIL_ZSTD_SNAPSHOT_H
#define COOKINGFOIL_ZSTD_SNAPSHOT_H
/* Saving a zstd decoder between blocks (buffer-less decoding) and putting it
 * back later; see ZstdSnapshot.c. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
size_t ZSTDR_snapshotSize(void);
/* 1 when the next input is a block header: everything before is decoded. */
int ZSTDR_atBlockBoundary(const ZSTD_DCtx *dctx);
/* 0 on success; -1 when not at a block boundary or `out` is too small. */
int ZSTDR_save(const ZSTD_DCtx *dctx, void *out, size_t outSize);
/* A fresh decoder at the saved point, the last `windowSize` bytes of output
 * at `window` (which must stay in place, the next output written right after
 * it). -1 when the snapshot is not one this build wrote. */
int ZSTDR_restore(ZSTD_DCtx *dctx, const void *in, size_t inSize, const void *window, size_t windowSize);
#ifdef __cplusplus
}
#endif
#endif
