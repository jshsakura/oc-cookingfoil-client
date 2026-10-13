/* A zstd decoder's state between two blocks, saved and put back, so a
 * frame cut off part way can go on decoding from that block. It reads the
 * decoder's internals, which is why zstd is vendored at a fixed version
 * (third_party/zstd, 1.5.7): the layout below must match it exactly. */
#define ZSTD_STATIC_LINKING_ONLY
#include "ZstdSnapshot.h"
#include "decompress/zstd_decompress_internal.h"
#include <string.h>

#define SNAPSHOT_MAGIC 0x5a534e31u /* "ZSN1" */

/* What a block needs from the ones before it. The table pointers are saved
 * as the tables they point at (the decoder's own or zstd's defaults), and
 * restored into the decoder's own space. */
typedef struct {
    U32 magic;
    U32 version;
    ZSTD_seqSymbol LLTable[SEQSYMBOL_TABLE_SIZE(LLFSELog)];
    ZSTD_seqSymbol OFTable[SEQSYMBOL_TABLE_SIZE(OffFSELog)];
    ZSTD_seqSymbol MLTable[SEQSYMBOL_TABLE_SIZE(MLFSELog)];
    HUF_DTable hufTable[HUF_DTABLE_SIZE(ZSTD_HUFFDTABLE_CAPACITY_LOG)];
    U32 rep[ZSTD_REP_NUM];
    ZSTD_FrameHeader fParams;
    U64 processedCSize;
    U64 decodedSize;
    U64 expected;
    U64 headerSize;
    U32 stage;
    U32 bType;
    U32 litEntropy;
    U32 fseEntropy;
    U32 validateChecksum;
    U32 isFrameDecompression;
    U32 format;
    XXH64_state_t xxhState;
} Snapshot;

size_t ZSTDR_snapshotSize(void) { return sizeof(Snapshot); }

int ZSTDR_atBlockBoundary(const ZSTD_DCtx *dctx) {
    return dctx->stage == ZSTDds_decodeBlockHeader;
}

int ZSTDR_save(const ZSTD_DCtx *dctx, void *out, size_t outSize) {
    Snapshot s;
    if (outSize < sizeof(Snapshot) || dctx->stage != ZSTDds_decodeBlockHeader)
        return -1;
    memset(&s, 0, sizeof(s));
    s.magic = SNAPSHOT_MAGIC;
    s.version = ZSTD_VERSION_NUMBER;
    /* The tables the next block may repeat: wherever they live now. */
    if (dctx->LLTptr) memcpy(s.LLTable, dctx->LLTptr, sizeof(s.LLTable));
    if (dctx->OFTptr) memcpy(s.OFTable, dctx->OFTptr, sizeof(s.OFTable));
    if (dctx->MLTptr) memcpy(s.MLTable, dctx->MLTptr, sizeof(s.MLTable));
    if (dctx->HUFptr) memcpy(s.hufTable, dctx->HUFptr, sizeof(s.hufTable));
    memcpy(s.rep, dctx->entropy.rep, sizeof(s.rep));
    s.fParams = dctx->fParams;
    s.processedCSize = dctx->processedCSize;
    s.decodedSize = dctx->decodedSize;
    s.expected = dctx->expected;
    s.headerSize = dctx->headerSize;
    s.stage = (U32)dctx->stage;
    s.bType = (U32)dctx->bType;
    s.litEntropy = dctx->litEntropy;
    s.fseEntropy = dctx->fseEntropy;
    s.validateChecksum = dctx->validateChecksum;
    s.isFrameDecompression = (U32)dctx->isFrameDecompression;
    s.format = (U32)dctx->format;
    s.xxhState = dctx->xxhState;
    memcpy(out, &s, sizeof(s));
    return 0;
}

int ZSTDR_restore(ZSTD_DCtx *dctx, const void *in, size_t inSize, const void *window, size_t windowSize) {
    Snapshot s;
    if (inSize != sizeof(Snapshot))
        return -1;
    memcpy(&s, in, sizeof(s));
    if (s.magic != SNAPSHOT_MAGIC || s.version != ZSTD_VERSION_NUMBER || s.stage != (U32)ZSTDds_decodeBlockHeader)
        return -1;
    if (ZSTD_isError(ZSTD_decompressBegin(dctx)))
        return -1;
    memcpy(dctx->entropy.LLTable, s.LLTable, sizeof(s.LLTable));
    memcpy(dctx->entropy.OFTable, s.OFTable, sizeof(s.OFTable));
    memcpy(dctx->entropy.MLTable, s.MLTable, sizeof(s.MLTable));
    memcpy(dctx->entropy.hufTable, s.hufTable, sizeof(s.hufTable));
    memcpy(dctx->entropy.rep, s.rep, sizeof(s.rep));
    dctx->LLTptr = dctx->entropy.LLTable;
    dctx->OFTptr = dctx->entropy.OFTable;
    dctx->MLTptr = dctx->entropy.MLTable;
    dctx->HUFptr = dctx->entropy.hufTable;
    dctx->fParams = s.fParams;
    dctx->processedCSize = s.processedCSize;
    dctx->decodedSize = s.decodedSize;
    dctx->expected = (size_t)s.expected;
    dctx->headerSize = (size_t)s.headerSize;
    dctx->stage = (ZSTD_dStage)s.stage;
    dctx->bType = (blockType_e)s.bType;
    dctx->litEntropy = s.litEntropy;
    dctx->fseEntropy = s.fseEntropy;
    dctx->validateChecksum = s.validateChecksum;
    dctx->isFrameDecompression = (int)s.isFrameDecompression;
    dctx->format = (ZSTD_format_e)s.format;
    dctx->xxhState = s.xxhState;
    /* The window: what was written last, now in the caller's buffer. The
     * next block's output goes right after it. */
    dctx->prefixStart = window;
    dctx->virtualStart = window;
    dctx->dictEnd = window;
    dctx->previousDstEnd = (const char *)window + windowSize;
    return 0;
}
