/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_TRACE_LOG_H
#define ZSTD_TRACE_LOG_H

#ifndef ZSTD_TRACE_LOG
#  define ZSTD_TRACE_LOG 0
#endif

#if ZSTD_TRACE_LOG

#include <stddef.h>
#include "mem.h"

/* Forward declarations */
struct ZSTD_CCtx_params_s;

/*--- Match length histogram bins ---*/
typedef struct {
    U32 ml_3_4;
    U32 ml_5_8;
    U32 ml_9_16;
    U32 ml_17_32;
    U32 ml_33_plus;
} ZSTD_traceLog_MLHist;

/*--- Offset distance histogram bins ---*/
typedef struct {
    U32 short_le256;
    U32 medium_257_4096;
    U32 long_gt4096;
} ZSTD_traceLog_OffHist;

/*--- Literal run length histogram bins ---*/
typedef struct {
    U32 litRun_0;
    U32 litRun_1_3;
    U32 litRun_4_15;
    U32 litRun_16_63;
    U32 litRun_64_plus;
} ZSTD_traceLog_LitRunHist;

/*--- Per-block stats ---*/
typedef struct {
    U32 blockIndex;
    U32 srcSize;
    U32 cSize;
    U32 nbSequences;
    U32 totalLitBytes;
    U32 totalMatchBytes;
    U32 repcodeCount;
    U32 ldmSequences;
} ZSTD_traceLog_BlockStats;

/*--- Timer (platform-specific internals) ---*/
typedef struct {
    S64 ticksStart;
    S64 frequency;
} ZSTD_traceLog_Timer;

/*--- Maximum blocks tracked per operation ---*/
#define ZSTD_TRACE_LOG_MAX_BLOCKS 1024

/*--- Function call log ---*/
#define ZSTD_TRACE_LOG_MAX_CALLS 256

typedef struct {
    const char* funcName;
    U32 relativeUs;
} ZSTD_traceLog_CallEntry;

/*--- Operation context: one per compress/decompress call ---*/
typedef struct {
    /* Identity */
    U64 operationId;
    U64 threadId;
    U64 timestampUs;

    /* Sizes */
    U64 srcSize;
    U64 dstSize;

    /* Parameters (compression only) */
    int compressionLevel;
    U32 strategy;
    U32 windowLog;
    U32 hashLog;
    U32 chainLog;
    U32 searchLog;
    U32 minMatch;
    U32 targetLength;
    int nbWorkers;
    U32 dictID;
    U64 dictSize;
    int ldmEnabled;
    U32 ldmHashLog;
    U32 ldmMinMatch;
    U32 ldmBucketSizeLog;
    U32 ldmHashRateLog;

    /* Sequence-level aggregates */
    U64 totalSequences;
    U64 totalLitBytes;
    U64 totalMatchBytes;
    U64 repcodeHits;
    U64 ldmSequences;
    ZSTD_traceLog_MLHist mlHist;
    ZSTD_traceLog_OffHist offHist;

    /* Strategy-specific stats */
    U64 lazyAttempts;
    U64 lazyImprovements;
    U64 lazyImprovementDeltaSum;
    U64 totalSearches;
    U64 totalChainDepthConsumed;
    U64 optPositionsEvaluated;
    U64 optMatchesFound;
    U64 maxOffsetUsed;
    U32 rawBlocks;
    U32 rleBlocks;
    U32 compressedBlocks;
    ZSTD_traceLog_LitRunHist litRunHist;

    /* Per-block stats */
    U32 nbBlocks;
    U32 blockStatsCapacity;
    ZSTD_traceLog_BlockStats* blockStats;

    /* Timing */
    ZSTD_traceLog_Timer timer;
    U64 durationUs;

    /* Operation metadata */
    int isCompression;
    int streaming;
    int active;
    const char* entryApi;

    /* Function call log (ring buffer) */
    U32 callLogCount;
    U32 callLogWrapped;
    ZSTD_traceLog_CallEntry callLog[ZSTD_TRACE_LOG_MAX_CALLS];
} ZSTD_traceLog_OpCtx;

/*====== API ======*/

int  ZSTD_traceLog_begin(ZSTD_traceLog_OpCtx* ctx, int isCompression);

void ZSTD_traceLog_setParams(ZSTD_traceLog_OpCtx* ctx,
                             const struct ZSTD_CCtx_params_s* params);

void ZSTD_traceLog_addBlock(ZSTD_traceLog_OpCtx* ctx,
                            const void* seqStore,
                            U32 blockSrcSize,
                            U32 blockCSize,
                            U32 ldmSequencesInBlock);

void ZSTD_traceLog_end(ZSTD_traceLog_OpCtx* ctx,
                       U64 finalSrcSize,
                       U64 finalDstSize);

void ZSTD_traceLog_endDecompress(ZSTD_traceLog_OpCtx* ctx,
                                 U64 uncompressedSize,
                                 U64 compressedSize);

void ZSTD_traceLog_logFunc(ZSTD_traceLog_OpCtx* ctx, const char* funcName);

#define ZSTD_TRACE_LOG_FUNC(ctx, name) ZSTD_traceLog_logFunc(ctx, name)

/* Strategy-specific recording */
void ZSTD_traceLog_recordLazyAttempt(ZSTD_traceLog_OpCtx* ctx);
void ZSTD_traceLog_recordLazyImprovement(ZSTD_traceLog_OpCtx* ctx, U32 oldLen, U32 newLen);
void ZSTD_traceLog_recordSearchDepth(ZSTD_traceLog_OpCtx* ctx, U32 depthConsumed);
void ZSTD_traceLog_recordOptPositions(ZSTD_traceLog_OpCtx* ctx, U32 positions, U32 matches);
void ZSTD_traceLog_recordBlockType(ZSTD_traceLog_OpCtx* ctx, int blockType);

#else /* ZSTD_TRACE_LOG == 0 */

typedef int ZSTD_traceLog_OpCtx;
#define ZSTD_traceLog_begin(ctx, isc)            ((void)(ctx), (void)(isc), 0)
#define ZSTD_traceLog_setParams(ctx, p)          ((void)(ctx), (void)(p))
#define ZSTD_traceLog_addBlock(ctx, ss, bs, cs, ldm) \
    ((void)(ctx), (void)(ss), (void)(bs), (void)(cs), (void)(ldm))
#define ZSTD_traceLog_end(ctx, s, d)             ((void)(ctx), (void)(s), (void)(d))
#define ZSTD_traceLog_endDecompress(ctx, u, c)   ((void)(ctx), (void)(u), (void)(c))
#define ZSTD_TRACE_LOG_FUNC(ctx, name)           ((void)(ctx), (void)(name))
#define ZSTD_traceLog_recordLazyAttempt(ctx)     ((void)(ctx))
#define ZSTD_traceLog_recordLazyImprovement(ctx,o,n) ((void)(ctx),(void)(o),(void)(n))
#define ZSTD_traceLog_recordSearchDepth(ctx,d)   ((void)(ctx),(void)(d))
#define ZSTD_traceLog_recordOptPositions(ctx,p,m) ((void)(ctx),(void)(p),(void)(m))
#define ZSTD_traceLog_recordBlockType(ctx,t)     ((void)(ctx),(void)(t))

#endif /* ZSTD_TRACE_LOG */
#endif /* ZSTD_TRACE_LOG_H */
