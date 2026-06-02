/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "zstd_trace_log.h"

#if ZSTD_TRACE_LOG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*=== Platform-specific includes (before zstd headers to avoid ERROR macro conflict) ===*/
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  ifdef ERROR
#    undef ERROR
#  endif
#else
#  include <time.h>
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/stat.h>
#endif

#include "mem.h"
#include "zstd_internal.h"
#include "../compress/zstd_compress_internal.h"

/*=== Cross-platform timing ===*/

static S64 ZSTD_traceLog_getFrequency(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return (S64)freq.QuadPart;
#else
    return (S64)1000000000LL;
#endif
}

static S64 ZSTD_traceLog_getTicks(void)
{
#if defined(_WIN32)
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    return (S64)ticks.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (S64)ts.tv_sec * 1000000000LL + (S64)ts.tv_nsec;
#endif
}

static U64 ZSTD_traceLog_getThreadId(void)
{
#if defined(_WIN32)
    return (U64)GetCurrentThreadId();
#else
    return (U64)(size_t)pthread_self();
#endif
}

static U64 ZSTD_traceLog_getTimestampUs(void)
{
#if defined(_WIN32)
    FILETIME ft;
    U64 t;
    GetSystemTimeAsFileTime(&ft);
    t = ((U64)ft.dwHighDateTime << 32) | (U64)ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (U64)ts.tv_sec * 1000000ULL + (U64)ts.tv_nsec / 1000;
#endif
}

/*=== Atomic operation ID ===*/

static U64 ZSTD_traceLog_nextOpId(void)
{
#if defined(_WIN32)
    static volatile LONGLONG g_opCounter = 0;
    return (U64)InterlockedIncrement64(&g_opCounter);
#elif defined(__GNUC__) || defined(__clang__)
    static U64 g_opCounter = 0;
    return __atomic_fetch_add(&g_opCounter, 1, __ATOMIC_RELAXED) + 1;
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
    #include <stdatomic.h>
    static _Atomic U64 g_opCounter = 0;
    return atomic_fetch_add_explicit(&g_opCounter, 1, memory_order_relaxed) + 1;
#else
    /* Fallback: not thread-safe. Acceptable because this path is only reached
     * on compilers without atomics (rare), and operationId is only used for
     * filename uniqueness — a duplicate is harmless (file gets overwritten). */
    static U64 g_opCounter = 0;
    return ++g_opCounter;
#endif
}

/*=== API Implementation ===*/

int ZSTD_traceLog_begin(ZSTD_traceLog_OpCtx* ctx, int isCompression)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->isCompression = isCompression;
    ctx->operationId = ZSTD_traceLog_nextOpId();
    ctx->threadId = ZSTD_traceLog_getThreadId();
    ctx->timestampUs = ZSTD_traceLog_getTimestampUs();
    ctx->timer.frequency = ZSTD_traceLog_getFrequency();
    ctx->timer.ticksStart = ZSTD_traceLog_getTicks();

    ctx->blockStatsCapacity = 64;
    ctx->blockStats = (ZSTD_traceLog_BlockStats*)malloc(
        ctx->blockStatsCapacity * sizeof(ZSTD_traceLog_BlockStats));
    if (ctx->blockStats == NULL) {
        ctx->active = 0;
        return -1;
    }
    ctx->active = 1;
    return 0;
}

void ZSTD_traceLog_setParams(ZSTD_traceLog_OpCtx* ctx,
                             const struct ZSTD_CCtx_params_s* params)
{
    if (!ctx->active) return;
    ctx->compressionLevel = params->compressionLevel;
    ctx->strategy = (U32)params->cParams.strategy;
    ctx->windowLog = params->cParams.windowLog;
    ctx->hashLog = params->cParams.hashLog;
    ctx->chainLog = params->cParams.chainLog;
    ctx->searchLog = params->cParams.searchLog;
    ctx->minMatch = params->cParams.minMatch;
    ctx->targetLength = params->cParams.targetLength;
    ctx->nbWorkers = params->nbWorkers;
    ctx->ldmEnabled = (params->ldmParams.enableLdm == ZSTD_ps_enable);
    ctx->ldmHashLog = params->ldmParams.hashLog;
    ctx->ldmMinMatch = params->ldmParams.minMatchLength;
    ctx->ldmBucketSizeLog = params->ldmParams.bucketSizeLog;
    ctx->ldmHashRateLog = params->ldmParams.hashRateLog;
}

void ZSTD_traceLog_logFunc(ZSTD_traceLog_OpCtx* ctx, const char* funcName)
{
    U32 idx;
    S64 now;
    U64 relUs;

    if (!ctx->active) return;

    now = ZSTD_traceLog_getTicks();
    if (ctx->timer.frequency > 0) {
        relUs = (U64)((now - ctx->timer.ticksStart) * 1000000LL / ctx->timer.frequency);
    } else {
        relUs = 0;
    }

    idx = ctx->callLogCount % ZSTD_TRACE_LOG_MAX_CALLS;
    ctx->callLog[idx].funcName = funcName;
    ctx->callLog[idx].relativeUs = (U32)relUs;
    ctx->callLogCount++;
    if (ctx->callLogCount > ZSTD_TRACE_LOG_MAX_CALLS) {
        ctx->callLogWrapped = 1;
    }
}

void ZSTD_traceLog_addBlock(ZSTD_traceLog_OpCtx* ctx,
                            const void* seqStoreVoid,
                            U32 blockSrcSize,
                            U32 blockCSize,
                            U32 ldmSequencesInBlock)
{
    const SeqStore_t* seqStore;
    U32 nbSeqs;
    U32 litBytesBlock = 0;
    U32 matchBytesBlock = 0;
    U32 repcodeBlock = 0;
    U32 i;
    ZSTD_traceLog_BlockStats* bs;

    if (!ctx->active) return;

    seqStore = (const SeqStore_t*)seqStoreVoid;
    nbSeqs = (U32)(seqStore->sequences - seqStore->sequencesStart);

    /* Grow blockStats if needed */
    if (ctx->nbBlocks >= ctx->blockStatsCapacity) {
        if (ctx->blockStatsCapacity >= ZSTD_TRACE_LOG_MAX_BLOCKS) {
            /* Cap reached; just accumulate totals, skip per-block record */
            goto accumulate_only;
        }
        {   U32 newCap = ctx->blockStatsCapacity * 2;
            ZSTD_traceLog_BlockStats* newBuf;
            if (newCap > ZSTD_TRACE_LOG_MAX_BLOCKS)
                newCap = ZSTD_TRACE_LOG_MAX_BLOCKS;
            newBuf = (ZSTD_traceLog_BlockStats*)realloc(
                ctx->blockStats, newCap * sizeof(ZSTD_traceLog_BlockStats));
            if (newBuf == NULL) goto accumulate_only;
            ctx->blockStats = newBuf;
            ctx->blockStatsCapacity = newCap;
        }
    }

    bs = &ctx->blockStats[ctx->nbBlocks];
    ctx->nbBlocks++;
    bs->blockIndex = ctx->nbBlocks - 1;
    bs->srcSize = blockSrcSize;
    bs->cSize = blockCSize;
    bs->nbSequences = nbSeqs;
    bs->ldmSequences = ldmSequencesInBlock;

accumulate_only:
    for (i = 0; i < nbSeqs; i++) {
        const SeqDef* seq = seqStore->sequencesStart + i;
        ZSTD_SequenceLength seqLen = ZSTD_getSequenceLength(seqStore, seq);
        U32 litLen = seqLen.litLength;
        U32 matchLen = seqLen.matchLength;
        U32 offBase = seq->offBase;

        litBytesBlock += litLen;
        matchBytesBlock += matchLen;

        /* Literal run length histogram */
        if (litLen == 0)          ctx->litRunHist.litRun_0++;
        else if (litLen <= 3)     ctx->litRunHist.litRun_1_3++;
        else if (litLen <= 15)    ctx->litRunHist.litRun_4_15++;
        else if (litLen <= 63)    ctx->litRunHist.litRun_16_63++;
        else                      ctx->litRunHist.litRun_64_plus++;

        /* Repcode detection */
        if (offBase <= ZSTD_REP_NUM) {
            repcodeBlock++;
        }

        /* Match length histogram */
        if (matchLen <= 4)        ctx->mlHist.ml_3_4++;
        else if (matchLen <= 8)   ctx->mlHist.ml_5_8++;
        else if (matchLen <= 16)  ctx->mlHist.ml_9_16++;
        else if (matchLen <= 32)  ctx->mlHist.ml_17_32++;
        else                      ctx->mlHist.ml_33_plus++;

        /* Offset histogram (non-repcode only) */
        if (offBase > ZSTD_REP_NUM) {
            U32 realOffset = offBase - ZSTD_REP_NUM;
            if (realOffset <= 256)        ctx->offHist.short_le256++;
            else if (realOffset <= 4096)  ctx->offHist.medium_257_4096++;
            else                          ctx->offHist.long_gt4096++;
            /* Track max offset used */
            if ((U64)realOffset > ctx->maxOffsetUsed)
                ctx->maxOffsetUsed = (U64)realOffset;
        }
    }

    /* Per-block record (only if we didn't skip due to cap) */
    if (ctx->nbBlocks > 0 && ctx->nbBlocks <= ctx->blockStatsCapacity) {
        ZSTD_traceLog_BlockStats* lastBs = &ctx->blockStats[ctx->nbBlocks - 1];
        lastBs->totalLitBytes = litBytesBlock;
        lastBs->totalMatchBytes = matchBytesBlock;
        lastBs->repcodeCount = repcodeBlock;
    }

    /* Accumulate operation-level totals */
    ctx->totalSequences += nbSeqs;
    ctx->totalLitBytes += litBytesBlock;
    ctx->totalMatchBytes += matchBytesBlock;
    ctx->repcodeHits += repcodeBlock;
    ctx->ldmSequences += ldmSequencesInBlock;
}

/*=== Strategy-specific recording ===*/

void ZSTD_traceLog_recordLazyAttempt(ZSTD_traceLog_OpCtx* ctx)
{
    if (!ctx || !ctx->active) return;
    ctx->lazyAttempts++;
}

void ZSTD_traceLog_recordLazyImprovement(ZSTD_traceLog_OpCtx* ctx, U32 oldLen, U32 newLen)
{
    if (!ctx || !ctx->active) return;
    ctx->lazyImprovements++;
    if (newLen > oldLen)
        ctx->lazyImprovementDeltaSum += (U64)(newLen - oldLen);
}

void ZSTD_traceLog_recordSearchDepth(ZSTD_traceLog_OpCtx* ctx, U32 depthConsumed)
{
    if (!ctx || !ctx->active) return;
    ctx->totalSearches++;
    ctx->totalChainDepthConsumed += depthConsumed;
}

void ZSTD_traceLog_recordOptPositions(ZSTD_traceLog_OpCtx* ctx, U32 positions, U32 matches)
{
    if (!ctx || !ctx->active) return;
    ctx->optPositionsEvaluated += positions;
    ctx->optMatchesFound += matches;
}

void ZSTD_traceLog_recordBlockType(ZSTD_traceLog_OpCtx* ctx, int blockType)
{
    if (!ctx || !ctx->active) return;
    switch (blockType) {
        case 0: ctx->rawBlocks++; break;
        case 1: ctx->rleBlocks++; break;
        case 2: ctx->compressedBlocks++; break;
    }
}

/*=== JSON Output ===*/

static void ZSTD_traceLog_writeTimestamp(FILE* f, U64 timestampUs)
{
    U64 secs = timestampUs / 1000000;
    U64 usRemainder = timestampUs % 1000000;
    U64 hours, minutes, seconds;
    U64 y, m, d;
    U64 totalDays;

    seconds = secs % 60; secs /= 60;
    minutes = secs % 60; secs /= 60;
    hours = secs % 24;
    totalDays = secs / 24;

    /* Simple civil date from days since epoch */
    {   U64 z = totalDays + 719468;
        U64 era = z / 146097;
        U64 doe = z - era * 146097;
        U64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
        U64 doy, mp;
        y = yoe + era * 400;
        doy = doe - (365*yoe + yoe/4 - yoe/100);
        mp = (5*doy + 2) / 153;
        d = doy - (153*mp + 2)/5 + 1;
        m = mp + (mp < 10 ? 3 : (U64)-9);
        if (m <= 2) y++;
    }

    fprintf(f, "%04u-%02u-%02uT%02u:%02u:%02u.%06uZ",
            (unsigned)y, (unsigned)m, (unsigned)d,
            (unsigned)hours, (unsigned)minutes, (unsigned)seconds,
            (unsigned)usRemainder);
}

static const char* ZSTD_traceLog_strategyName(U32 strategy)
{
    switch (strategy) {
        case 1: return "fast";
        case 2: return "dfast";
        case 3: return "greedy";
        case 4: return "lazy";
        case 5: return "lazy2";
        case 6: return "btlazy2";
        case 7: return "btopt";
        case 8: return "btultra";
        case 9: return "btultra2";
        default: return "unknown";
    }
}

static void ZSTD_traceLog_writeJson(ZSTD_traceLog_OpCtx* ctx, FILE* f)
{
    double ratio = 0.0;
    double litToMatchFrac = 0.0;
    double repcodeRate = 0.0;
    double throughput = 0.0;
    U32 i;

    if (ctx->dstSize > 0 && ctx->isCompression)
        ratio = (double)ctx->srcSize / (double)ctx->dstSize;
    else if (ctx->srcSize > 0 && !ctx->isCompression)
        ratio = (double)ctx->dstSize / (double)ctx->srcSize;

    if (ctx->totalMatchBytes > 0)
        litToMatchFrac = (double)ctx->totalLitBytes / (double)ctx->totalMatchBytes;
    if (ctx->totalSequences > 0)
        repcodeRate = (double)ctx->repcodeHits / (double)ctx->totalSequences;
    if (ctx->durationUs > 0) {
        U64 bytes = ctx->isCompression ? ctx->srcSize : ctx->dstSize;
        throughput = (double)bytes / (double)ctx->durationUs;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": \"1.0\",\n");
    fprintf(f, "  \"operation\": \"%s\",\n", ctx->isCompression ? "compress" : "decompress");
    fprintf(f, "  \"entryApi\": \"%s\",\n", ctx->entryApi ? ctx->entryApi : "unknown");
    fprintf(f, "  \"operationId\": %llu,\n", (unsigned long long)ctx->operationId);
    fprintf(f, "  \"threadId\": %llu,\n", (unsigned long long)ctx->threadId);
    fprintf(f, "  \"timestamp\": \"");
    ZSTD_traceLog_writeTimestamp(f, ctx->timestampUs);
    fprintf(f, "\",\n");
    fprintf(f, "  \"streaming\": %s,\n", ctx->streaming ? "true" : "false");

    /* Sizes */
    fprintf(f, "  \"sizes\": {\n");
    fprintf(f, "    \"srcBytes\": %llu,\n", (unsigned long long)ctx->srcSize);
    fprintf(f, "    \"dstBytes\": %llu,\n", (unsigned long long)ctx->dstSize);
    fprintf(f, "    \"ratio\": %.4f\n", ratio);
    fprintf(f, "  },\n");

    /* Parameters (compression only) */
    if (ctx->isCompression) {
        fprintf(f, "  \"params\": {\n");
        fprintf(f, "    \"level\": %d,\n", ctx->compressionLevel);
        fprintf(f, "    \"strategy\": \"%s\",\n", ZSTD_traceLog_strategyName(ctx->strategy));
        fprintf(f, "    \"windowLog\": %u,\n", (unsigned)ctx->windowLog);
        fprintf(f, "    \"hashLog\": %u,\n", (unsigned)ctx->hashLog);
        fprintf(f, "    \"chainLog\": %u,\n", (unsigned)ctx->chainLog);
        fprintf(f, "    \"searchLog\": %u,\n", (unsigned)ctx->searchLog);
        fprintf(f, "    \"minMatch\": %u,\n", (unsigned)ctx->minMatch);
        fprintf(f, "    \"targetLength\": %u,\n", (unsigned)ctx->targetLength);
        fprintf(f, "    \"nbWorkers\": %d,\n", ctx->nbWorkers);
        fprintf(f, "    \"dictID\": %u,\n", (unsigned)ctx->dictID);
        fprintf(f, "    \"dictSize\": %llu,\n", (unsigned long long)ctx->dictSize);
        fprintf(f, "    \"ldm\": {\n");
        fprintf(f, "      \"enabled\": %s", ctx->ldmEnabled ? "true" : "false");
        if (ctx->ldmEnabled) {
            fprintf(f, ",\n");
            fprintf(f, "      \"hashLog\": %u,\n", (unsigned)ctx->ldmHashLog);
            fprintf(f, "      \"minMatch\": %u,\n", (unsigned)ctx->ldmMinMatch);
            fprintf(f, "      \"bucketSizeLog\": %u,\n", (unsigned)ctx->ldmBucketSizeLog);
            fprintf(f, "      \"hashRateLog\": %u\n", (unsigned)ctx->ldmHashRateLog);
        } else {
            fprintf(f, "\n");
        }
        fprintf(f, "    }\n");
        fprintf(f, "  },\n");
    }

    /* Sequence stats (compression only) */
    if (ctx->isCompression) {
        fprintf(f, "  \"sequences\": {\n");
        fprintf(f, "    \"totalCount\": %llu,\n", (unsigned long long)ctx->totalSequences);
        fprintf(f, "    \"totalLitBytes\": %llu,\n", (unsigned long long)ctx->totalLitBytes);
        fprintf(f, "    \"totalMatchBytes\": %llu,\n", (unsigned long long)ctx->totalMatchBytes);
        fprintf(f, "    \"litToMatchFraction\": %.4f,\n", litToMatchFrac);
        fprintf(f, "    \"repcodeHits\": %llu,\n", (unsigned long long)ctx->repcodeHits);
        fprintf(f, "    \"repcodeRate\": %.4f,\n", repcodeRate);
        fprintf(f, "    \"ldmSequences\": %llu,\n", (unsigned long long)ctx->ldmSequences);
        fprintf(f, "    \"matchLengthHistogram\": {\n");
        fprintf(f, "      \"3_4\": %u,\n", (unsigned)ctx->mlHist.ml_3_4);
        fprintf(f, "      \"5_8\": %u,\n", (unsigned)ctx->mlHist.ml_5_8);
        fprintf(f, "      \"9_16\": %u,\n", (unsigned)ctx->mlHist.ml_9_16);
        fprintf(f, "      \"17_32\": %u,\n", (unsigned)ctx->mlHist.ml_17_32);
        fprintf(f, "      \"33_plus\": %u\n", (unsigned)ctx->mlHist.ml_33_plus);
        fprintf(f, "    },\n");
        fprintf(f, "    \"offsetHistogram\": {\n");
        fprintf(f, "      \"short_le256\": %u,\n", (unsigned)ctx->offHist.short_le256);
        fprintf(f, "      \"medium_257_4096\": %u,\n", (unsigned)ctx->offHist.medium_257_4096);
        fprintf(f, "      \"long_gt4096\": %u\n", (unsigned)ctx->offHist.long_gt4096);
        fprintf(f, "    }\n");
        fprintf(f, "  },\n");

        /* Per-block stats */
        fprintf(f, "  \"blocks\": [\n");
        for (i = 0; i < ctx->nbBlocks && i < ctx->blockStatsCapacity; i++) {
            const ZSTD_traceLog_BlockStats* bs = &ctx->blockStats[i];
            double blockRatio = (bs->cSize > 0) ? (double)bs->srcSize / (double)bs->cSize : 0.0;
            fprintf(f, "    {");
            fprintf(f, "\"index\": %u, ", (unsigned)bs->blockIndex);
            fprintf(f, "\"srcSize\": %u, ", (unsigned)bs->srcSize);
            fprintf(f, "\"cSize\": %u, ", (unsigned)bs->cSize);
            fprintf(f, "\"ratio\": %.4f, ", blockRatio);
            fprintf(f, "\"nbSequences\": %u, ", (unsigned)bs->nbSequences);
            fprintf(f, "\"litBytes\": %u, ", (unsigned)bs->totalLitBytes);
            fprintf(f, "\"matchBytes\": %u, ", (unsigned)bs->totalMatchBytes);
            fprintf(f, "\"repcodeCount\": %u, ", (unsigned)bs->repcodeCount);
            fprintf(f, "\"ldmSequences\": %u", (unsigned)bs->ldmSequences);
            fprintf(f, "}%s\n", (i + 1 < ctx->nbBlocks && i + 1 < ctx->blockStatsCapacity) ? "," : "");
        }
        fprintf(f, "  ],\n");
    }

    /* Strategy-specific metrics (compression only) */
    if (ctx->isCompression) {
        fprintf(f, "  \"strategyMetrics\": {\n");
        fprintf(f, "    \"lazyAttempts\": %llu,\n", (unsigned long long)ctx->lazyAttempts);
        fprintf(f, "    \"lazyImprovements\": %llu,\n", (unsigned long long)ctx->lazyImprovements);
        fprintf(f, "    \"lazyImprovementDeltaSum\": %llu,\n", (unsigned long long)ctx->lazyImprovementDeltaSum);
        if (ctx->lazyImprovements > 0)
            fprintf(f, "    \"lazyAvgImprovement\": %.2f,\n",
                    (double)ctx->lazyImprovementDeltaSum / (double)ctx->lazyImprovements);
        fprintf(f, "    \"totalSearches\": %llu,\n", (unsigned long long)ctx->totalSearches);
        fprintf(f, "    \"totalChainDepthConsumed\": %llu,\n", (unsigned long long)ctx->totalChainDepthConsumed);
        if (ctx->totalSearches > 0)
            fprintf(f, "    \"avgSearchDepth\": %.2f,\n",
                    (double)ctx->totalChainDepthConsumed / (double)ctx->totalSearches);
        fprintf(f, "    \"optPositionsEvaluated\": %llu,\n", (unsigned long long)ctx->optPositionsEvaluated);
        fprintf(f, "    \"optMatchesFound\": %llu,\n", (unsigned long long)ctx->optMatchesFound);
        fprintf(f, "    \"maxOffsetUsed\": %llu,\n", (unsigned long long)ctx->maxOffsetUsed);
        if (ctx->srcSize > 0)
            fprintf(f, "    \"sequenceDensityPerKB\": %.2f,\n",
                    (double)ctx->totalSequences * 1024.0 / (double)ctx->srcSize);
        fprintf(f, "    \"blockTypes\": {\"raw\": %u, \"rle\": %u, \"compressed\": %u},\n",
                (unsigned)ctx->rawBlocks, (unsigned)ctx->rleBlocks, (unsigned)ctx->compressedBlocks);
        fprintf(f, "    \"litRunHistogram\": {\n");
        fprintf(f, "      \"0\": %u,\n", (unsigned)ctx->litRunHist.litRun_0);
        fprintf(f, "      \"1_3\": %u,\n", (unsigned)ctx->litRunHist.litRun_1_3);
        fprintf(f, "      \"4_15\": %u,\n", (unsigned)ctx->litRunHist.litRun_4_15);
        fprintf(f, "      \"16_63\": %u,\n", (unsigned)ctx->litRunHist.litRun_16_63);
        fprintf(f, "      \"64_plus\": %u\n", (unsigned)ctx->litRunHist.litRun_64_plus);
        fprintf(f, "    }\n");
        fprintf(f, "  },\n");
    }

    /* Timing */
    fprintf(f, "  \"timing\": {\n");
    fprintf(f, "    \"durationUs\": %llu,\n", (unsigned long long)ctx->durationUs);
    fprintf(f, "    \"throughputMBps\": %.2f\n", throughput);
    fprintf(f, "  },\n");

    /* Function call log */
    {   U32 logCount = ctx->callLogCount;
        U32 startIdx = 0;
        U32 numEntries = logCount;
        if (logCount > ZSTD_TRACE_LOG_MAX_CALLS) {
            numEntries = ZSTD_TRACE_LOG_MAX_CALLS;
            startIdx = logCount % ZSTD_TRACE_LOG_MAX_CALLS;
        }
        fprintf(f, "  \"callLog\": [\n");
        for (i = 0; i < numEntries; i++) {
            U32 idx = (startIdx + i) % ZSTD_TRACE_LOG_MAX_CALLS;
            const ZSTD_traceLog_CallEntry* entry = &ctx->callLog[idx];
            fprintf(f, "    {\"func\": \"%s\", \"relativeUs\": %u}%s\n",
                    entry->funcName,
                    (unsigned)entry->relativeUs,
                    (i + 1 < numEntries) ? "," : "");
        }
        fprintf(f, "  ]\n");
    }

    fprintf(f, "}\n");
}

static void ZSTD_traceLog_ensureDir(const char* dir)
{
#if defined(_WIN32)
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0755);
#endif
}

static FILE* ZSTD_traceLog_openFile(const ZSTD_traceLog_OpCtx* ctx)
{
    char path[512];
    const char* dir = getenv("ZSTD_TRACE_LOG_DIR");
    const char* op = ctx->isCompression ? "compress" : "decompress";
    const char* mode = ctx->streaming ? "_stream" : "";
    if (dir == NULL || dir[0] == '\0') dir = "zstd_traces";

    ZSTD_traceLog_ensureDir(dir);

#if defined(_WIN32)
    _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\zstd_trace_%s%s_%llu_%llu_%llu.json",
                dir, op, mode,
                (unsigned long long)ctx->timestampUs,
                (unsigned long long)ctx->threadId,
                (unsigned long long)ctx->operationId);
#else
    snprintf(path, sizeof(path),
             "%s/zstd_trace_%s%s_%llu_%llu_%llu.json",
             dir, op, mode,
             (unsigned long long)ctx->timestampUs,
             (unsigned long long)ctx->threadId,
             (unsigned long long)ctx->operationId);
#endif
    return fopen(path, "w");
}

void ZSTD_traceLog_end(ZSTD_traceLog_OpCtx* ctx,
                       U64 finalSrcSize,
                       U64 finalDstSize)
{
    FILE* f;
    S64 ticksEnd;

    if (!ctx->active) return;

    ctx->srcSize = finalSrcSize;
    ctx->dstSize = finalDstSize;

    /* Compute duration */
    ticksEnd = ZSTD_traceLog_getTicks();
    if (ctx->timer.frequency > 0) {
        ctx->durationUs = (U64)((ticksEnd - ctx->timer.ticksStart) * 1000000LL / ctx->timer.frequency);
    }

    f = ZSTD_traceLog_openFile(ctx);
    if (f != NULL) {
        ZSTD_traceLog_writeJson(ctx, f);
        fclose(f);
    }

    /* Cleanup */
    free(ctx->blockStats);
    ctx->blockStats = NULL;
    ctx->active = 0;
}

void ZSTD_traceLog_endDecompress(ZSTD_traceLog_OpCtx* ctx,
                                 U64 uncompressedSize,
                                 U64 compressedSize)
{
    FILE* f;
    S64 ticksEnd;

    if (!ctx->active) return;

    ctx->srcSize = compressedSize;
    ctx->dstSize = uncompressedSize;
    ctx->isCompression = 0;

    /* Compute duration */
    ticksEnd = ZSTD_traceLog_getTicks();
    if (ctx->timer.frequency > 0) {
        ctx->durationUs = (U64)((ticksEnd - ctx->timer.ticksStart) * 1000000LL / ctx->timer.frequency);
    }

    f = ZSTD_traceLog_openFile(ctx);
    if (f != NULL) {
        ZSTD_traceLog_writeJson(ctx, f);
        fclose(f);
    }

    /* Cleanup */
    free(ctx->blockStats);
    ctx->blockStats = NULL;
    ctx->active = 0;
}

#endif /* ZSTD_TRACE_LOG */
