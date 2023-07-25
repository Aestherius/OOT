#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libultraship/libultra.h>
#include "global.h"
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/audio/AudioCollection.h"
#include "soh/Enhancements/audio/AudioEditor.h"

#define MK_ASYNC_MSG(retData, tableType, id, status) (((retData) << 24) | ((tableType) << 16) | ((id) << 8) | (status))
#define ASYNC_TBLTYPE(v) ((u8)(v >> 16))
#define ASYNC_ID(v) ((u8)(v >> 8))
#define ASYNC_STATUS(v) ((u8)(v >> 0))

typedef enum {
    /* 0 */ LOAD_STATUS_WAITING,
    /* 1 */ LOAD_STATUS_START,
    /* 2 */ LOAD_STATUS_LOADING,
    /* 3 */ LOAD_STATUS_DONE
} SlowLoadStatus;

typedef struct {
    /* 0x00 */ s32 sampleBankId1;
    /* 0x04 */ s32 sampleBankId2;
    /* 0x08 */ intptr_t baseAddr1;
    /* 0x0C */ intptr_t baseAddr2;
    /* 0x10 */ u32 medium1;
    /* 0x14 */ u32 medium2;
} RelocInfo; // size = 0x18

// opaque type for unpatched sound font data (should maybe get rid of this?)
typedef void SoundFontData;

/* forward declarations */
s32 AudioLoad_SyncInitSeqPlayerInternal(s32 playerIdx, s32 seqId, s32 skipTicks);
SoundFontData* AudioLoad_SyncLoadFont(u32 fontId);
SoundFontSample* AudioLoad_GetFontSample(s32 fontId, s32 instId);
void AudioLoad_ProcessAsyncLoads(s32 resetStatus);
void AudioLoad_ProcessAsyncLoadUnkMedium(AudioAsyncLoad* asyncLoad, s32 resetStatus);
void AudioLoad_ProcessAsyncLoad(AudioAsyncLoad* asyncLoad, s32 resetStatus);
void AudioLoad_RelocateFontAndPreloadSamples(s32 fontId, SoundFontData* mem, RelocInfo* relocInfo, s32 temporary);
void AudioLoad_RelocateSample(SoundFontSound* sound, SoundFontData* mem, RelocInfo* relocInfo, int fontId);
void AudioLoad_DiscardFont(s32 fontId);
uintptr_t AudioLoad_TrySyncLoadSampleBank(u32 sampleBankId, u32* outMedium, s32 noLoad);
uintptr_t AudioLoad_SyncLoad(u32 tableType, u32 tableId, s32* didAllocate);
u32 AudioLoad_GetRealTableIndex(s32 tableType, u32 tableId);
void* AudioLoad_SearchCaches(s32 tableType, s32 id);
AudioTable* AudioLoad_GetLoadTable(s32 tableType);
void AudioLoad_SyncDma(uintptr_t devAddr, u8* addr, size_t size, s32 medium);
void AudioLoad_SyncDmaUnkMedium(uintptr_t devAddr, u8* addr, size_t size, s32 unkMediumParam);
s32 AudioLoad_Dma(OSIoMesg* mesg, u32 priority, s32 direction, uintptr_t devAddr, uintptr_t ramAddr, size_t size,
                  OSMesgQueue* reqQueue, s32 medium, const char* dmaFuncType);
void* AudioLoad_AsyncLoadInner(s32 tableType, s32 id, s32 nChunks, s32 retData, OSMesgQueue* retQueue);
AudioAsyncLoad* AudioLoad_StartAsyncLoadUnkMedium(s32 unkMediumParam, uintptr_t devAddr, uintptr_t ramAddr,
                                                  size_t size, s32 medium,
                                                  s32 nChunks, OSMesgQueue* retQueue, s32 retMsg);
AudioAsyncLoad* AudioLoad_StartAsyncLoad(uintptr_t devAddr, uintptr_t ramAddr, size_t size, s32 medium, s32 nChunks,
                                         OSMesgQueue* retQueue, s32 retMsg);
void AudioLoad_AsyncDma(AudioAsyncLoad* asyncLoad, size_t size);
void AudioLoad_AsyncDmaUnkMedium(uintptr_t devAddr, uintptr_t ramAddr, size_t size, s16 arg3);
u8* AudioLoad_SyncLoadSeq(s32 seqId);
s32 AudioLoad_ProcessSamplePreloads(s32 resetStatus);
void AudioLoad_DmaSlowCopy(AudioSlowLoad* slowLoad, size_t size);
void AudioLoad_ProcessSlowLoads(s32 resetStatus);
void AudioLoad_DmaSlowCopyUnkMedium(uintptr_t devAddr, uintptr_t ramAddr, size_t size, s32 arg3);

OSMesgQueue sScriptLoadQueue;
OSMesg sScriptLoadMesgBuf[0x10];
s8* sScriptLoadDonePointers[0x10];
s32 sAudioLoadPad1[2]; // file padding

s32 D_8016B780;
s32 sAudioLoadPad2[4]; // double file padding?

DmaHandler sDmaHandler = osEPiStartDma;
void* sUnusedHandler = NULL;

s32 gAudioContextInitialized = false;

char** sequenceMap;
size_t sequenceMapSize;
// A map of authentic sequence IDs to their cache policies, for use with sequence swapping.
u8 seqCachePolicyMap[MAX_AUTHENTIC_SEQID];
char* fontMap[256];

uintptr_t fontStart;
uint32_t fontOffsets[8192];

void AudioLoad_DecreaseSampleDmaTtls(void) {
    u32 i;

    for (i = 0; i < gAudioCtx.sampleDmaListSize1; i++) {
        SampleDma* dma = &gAudioCtx.sampleDmas[i];

        if (dma->ttl != 0) {
            dma->ttl--;
            if (dma->ttl == 0) {
                dma->reuseIndex = gAudioCtx.sampleDmaReuseQueue1WrPos;
                gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1WrPos] = i;
                gAudioCtx.sampleDmaReuseQueue1WrPos++;
            }
        }
    }

    for (i = gAudioCtx.sampleDmaListSize1; i < gAudioCtx.sampleDmaCount; i++) {
        SampleDma* dma = &gAudioCtx.sampleDmas[i];

        if (dma->ttl != 0) {
            dma->ttl--;
            if (dma->ttl == 0) {
                dma->reuseIndex = gAudioCtx.sampleDmaReuseQueue2WrPos;
                gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2WrPos] = i;
                gAudioCtx.sampleDmaReuseQueue2WrPos++;
            }
        }
    }

    gAudioCtx.unused2628 = 0;
}

uintptr_t AudioLoad_DmaSampleData(uintptr_t devAddr, size_t size, s32 arg2, u8* dmaIndexRef, s32 medium) {
    s32 pad1;
    SampleDma* dma;
    s32 hasDma = false;
    uintptr_t dmaDevAddr;
    u32 pad2;
    u32 dmaIndex;
    u32 transfer;
    s32 bufferPos;
    u32 i;

    if (arg2 != 0 || *dmaIndexRef >= gAudioCtx.sampleDmaListSize1) {
        for (i = gAudioCtx.sampleDmaListSize1; i < gAudioCtx.sampleDmaCount; i++) {
            dma = &gAudioCtx.sampleDmas[i];
            bufferPos = devAddr - dma->devAddr;
            if (0 <= bufferPos && (u32)bufferPos <= dma->size - size) {
                // We already have a DMA request for this memory range.
                if (dma->ttl == 0 &&
                    gAudioCtx.sampleDmaReuseQueue2RdPos != gAudioCtx.sampleDmaReuseQueue2WrPos) {
                    // Move the DMA out of the reuse queue, by swapping it with the
                    // read pos, and then incrementing the read pos.
                    if (dma->reuseIndex != gAudioCtx.sampleDmaReuseQueue2RdPos) {
                        gAudioCtx.sampleDmaReuseQueue2[dma->reuseIndex] =
                            gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2RdPos];
                        gAudioCtx
                            .sampleDmas[gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2RdPos]]
                            .reuseIndex = dma->reuseIndex;
                    }
                    gAudioCtx.sampleDmaReuseQueue2RdPos++;
                }
                dma->ttl = 32;
                *dmaIndexRef = (u8)i;
                return &dma->ramAddr[devAddr - dma->devAddr];
            }
        }

        if (arg2 == 0) {
            goto search_short_lived;
        }

        if (gAudioCtx.sampleDmaReuseQueue2RdPos != gAudioCtx.sampleDmaReuseQueue2WrPos && arg2 != 0) {
            // Allocate a DMA from reuse queue 2, unless full.
            dmaIndex = gAudioCtx.sampleDmaReuseQueue2[gAudioCtx.sampleDmaReuseQueue2RdPos];
            gAudioCtx.sampleDmaReuseQueue2RdPos++;
            dma = gAudioCtx.sampleDmas + dmaIndex;
            hasDma = true;
        }
    } else {
    search_short_lived:
        dma = gAudioCtx.sampleDmas + *dmaIndexRef;
        i = 0;
    again:
        bufferPos = devAddr - dma->devAddr;
        if (0 <= bufferPos && (u32)bufferPos <= dma->size - size) {
            // We already have DMA for this memory range.
            if (dma->ttl == 0) {
                // Move the DMA out of the reuse queue, by swapping it with the
                // read pos, and then incrementing the read pos.
                if (dma->reuseIndex != gAudioCtx.sampleDmaReuseQueue1RdPos) {
                    gAudioCtx.sampleDmaReuseQueue1[dma->reuseIndex] =
                        gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1RdPos];
                    gAudioCtx
                        .sampleDmas[gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1RdPos]]
                        .reuseIndex = dma->reuseIndex;
                }
                gAudioCtx.sampleDmaReuseQueue1RdPos++;
            }
            dma->ttl = 2;
            return dma->ramAddr + (devAddr - dma->devAddr);
        }
        dma = gAudioCtx.sampleDmas + i++;
        if (i <= gAudioCtx.sampleDmaListSize1) {
            goto again;
        }
    }

    if (!hasDma) {
        if (gAudioCtx.sampleDmaReuseQueue1RdPos == gAudioCtx.sampleDmaReuseQueue1WrPos) {
            return NULL;
        }
        // Allocate a DMA from reuse queue 1.
        dmaIndex = gAudioCtx.sampleDmaReuseQueue1[gAudioCtx.sampleDmaReuseQueue1RdPos++];
        dma = gAudioCtx.sampleDmas + dmaIndex;
        hasDma = true;
    }

    transfer = dma->size;
    dmaDevAddr = devAddr & ~0xF;
    dma->ttl = 3;
    dma->devAddr = dmaDevAddr;
    dma->sizeUnused = transfer;
    AudioLoad_Dma(&gAudioCtx.currAudioFrameDmaIoMesgBuf[gAudioCtx.curAudioFrameDmaCount++], OS_MESG_PRI_NORMAL,
                  OS_READ, dmaDevAddr, dma->ramAddr, transfer, &gAudioCtx.currAudioFrameDmaQueue, medium,
                  "SUPERDMA");
    *dmaIndexRef = dmaIndex;

    return (devAddr - dmaDevAddr) + dma->ramAddr;
}

void AudioLoad_InitSampleDmaBuffers(s32 arg0) {
    SampleDma* dma;
    s32 i;
    s32 t2;
    s32 j;

    gAudioCtx.sampleDmaBufSize = gAudioCtx.sampleDmaBufSize1;
    gAudioCtx.sampleDmas =
        AudioHeap_Alloc(&gAudioCtx.notesAndBuffersPool,
                        4 * gAudioCtx.numNotes * sizeof(SampleDma) * gAudioCtx.audioBufferParameters.specUnk4);
    t2 = 3 * gAudioCtx.numNotes * gAudioCtx.audioBufferParameters.specUnk4;
    for (i = 0; i < t2; i++) {
        dma = &gAudioCtx.sampleDmas[gAudioCtx.sampleDmaCount];
        dma->ramAddr =
            AudioHeap_AllocAttemptExternal(&gAudioCtx.notesAndBuffersPool, gAudioCtx.sampleDmaBufSize);
        if (dma->ramAddr == NULL) {
            break;
        } else {
            AudioHeap_WritebackDCache(dma->ramAddr, gAudioCtx.sampleDmaBufSize);
            dma->size = gAudioCtx.sampleDmaBufSize;
            dma->devAddr = 0;
            dma->sizeUnused = 0;
            dma->unused = 0;
            dma->ttl = 0;
            gAudioCtx.sampleDmaCount++;
        }
    }

    for (i = 0; (u32)i < gAudioCtx.sampleDmaCount; i++) {
        gAudioCtx.sampleDmaReuseQueue1[i] = i;
        gAudioCtx.sampleDmas[i].reuseIndex = i;
    }

    for (i = gAudioCtx.sampleDmaCount; i < 0x100; i++) {
        gAudioCtx.sampleDmaReuseQueue1[i] = 0;
    }

    gAudioCtx.sampleDmaReuseQueue1RdPos = 0;
    gAudioCtx.sampleDmaReuseQueue1WrPos = gAudioCtx.sampleDmaCount;
    gAudioCtx.sampleDmaListSize1 = gAudioCtx.sampleDmaCount;
    gAudioCtx.sampleDmaBufSize = gAudioCtx.sampleDmaBufSize2;

    for (j = 0; j < gAudioCtx.numNotes; j++) {
        dma = &gAudioCtx.sampleDmas[gAudioCtx.sampleDmaCount];
        dma->ramAddr =
            AudioHeap_AllocAttemptExternal(&gAudioCtx.notesAndBuffersPool, gAudioCtx.sampleDmaBufSize);
        if (dma->ramAddr == NULL) {
            break;
        } else {
            AudioHeap_WritebackDCache(dma->ramAddr, gAudioCtx.sampleDmaBufSize);
            dma->size = gAudioCtx.sampleDmaBufSize;
            dma->devAddr = 0U;
            dma->sizeUnused = 0;
            dma->unused = 0;
            dma->ttl = 0;
            gAudioCtx.sampleDmaCount++;
        }
    }

    for (i = gAudioCtx.sampleDmaListSize1; (u32)i < gAudioCtx.sampleDmaCount; i++) {
        gAudioCtx.sampleDmaReuseQueue2[i - gAudioCtx.sampleDmaListSize1] = i;
        gAudioCtx.sampleDmas[i].reuseIndex = i - gAudioCtx.sampleDmaListSize1;
    }

    for (i = gAudioCtx.sampleDmaCount; i < 0x100; i++) {
        gAudioCtx.sampleDmaReuseQueue2[i] = gAudioCtx.sampleDmaListSize1;
    }

    gAudioCtx.sampleDmaReuseQueue2RdPos = 0;
    gAudioCtx.sampleDmaReuseQueue2WrPos = gAudioCtx.sampleDmaCount - gAudioCtx.sampleDmaListSize1;
}

s32 AudioLoad_IsFontLoadComplete(s32 fontId) {
    return true;
    if (fontId == 0xFF) {
        return true;

    } else if (gAudioCtx.fontLoadStatus[fontId] >= 2) {
        return true;
    } else if (gAudioCtx.fontLoadStatus[AudioLoad_GetRealTableIndex(FONT_TABLE, fontId)] >= 2) {
        return true;
    } else {
        return false;
    }
}

s32 AudioLoad_IsSeqLoadComplete(s32 seqId) {
    if (seqId == 0xFF) {
        return true;
    } else if (gAudioCtx.seqLoadStatus[seqId] >= 2) {
        return true;
    } else if (gAudioCtx.seqLoadStatus[AudioLoad_GetRealTableIndex(SEQUENCE_TABLE, seqId)] >= 2) {
        return true;
    } else {
        return false;
    }
}

s32 AudioLoad_IsSampleLoadComplete(s32 sampleBankId) {
    if (sampleBankId == 0xFF) {
        return true;
    } else if (gAudioCtx.sampleFontLoadStatus[sampleBankId] >= 2) {
        return true;
    } else if (gAudioCtx.sampleFontLoadStatus[AudioLoad_GetRealTableIndex(SAMPLE_TABLE, sampleBankId)] >= 2) {
        return true;
    } else {
        return false;
    }
}

void AudioLoad_SetFontLoadStatus(s32 fontId, s32 status) {
    if ((fontId != 0xFF) && (gAudioCtx.fontLoadStatus[fontId] != 5)) {
        gAudioCtx.fontLoadStatus[fontId] = status;
    }
}

void AudioLoad_SetSeqLoadStatus(s32 seqId, s32 status) {
    if ((seqId != 0xFF) && (gAudioCtx.seqLoadStatus[seqId] != 5)) {
        gAudioCtx.seqLoadStatus[seqId] = status;
    }
}

void AudioLoad_SetSampleFontLoadStatusAndApplyCaches(s32 sampleBankId, s32 status) {
    if (sampleBankId != 0xFF) {
        if (gAudioCtx.sampleFontLoadStatus[sampleBankId] != 5) {
            gAudioCtx.sampleFontLoadStatus[sampleBankId] = status;
        }

        if ((gAudioCtx.sampleFontLoadStatus[sampleBankId] == 5) ||
            (gAudioCtx.sampleFontLoadStatus[sampleBankId] == 2)) {
            AudioHeap_ApplySampleBankCache(sampleBankId);
        }
    }
}

void AudioLoad_SetSampleFontLoadStatus(s32 sampleBankId, s32 status) {
    if ((sampleBankId != 0xFF) && (gAudioCtx.sampleFontLoadStatus[sampleBankId] != 5)) {
        gAudioCtx.sampleFontLoadStatus[sampleBankId] = status;
    }
}

void AudioLoad_InitTable(AudioTable* table, uintptr_t romAddr, u16 unkMediumParam) {
    s32 i;

    table->unkMediumParam = unkMediumParam;
    table->romAddr = romAddr;

    for (i = 0; i < table->numEntries; i++) {
        if ((table->entries[i].size != 0) && (table->entries[i].medium == MEDIUM_CART)) {
            if (romAddr == fontStart)
                fontOffsets[i] = table->entries[i].romAddr;

            table->entries[i].romAddr += romAddr;
        }
    }
}

SoundFontData* AudioLoad_SyncLoadSeqFonts(s32 seqId, u32* outDefaultFontId) {
    char pad[0x8];
    s32 index;
    SoundFontData* font = NULL;
    s32 numFonts;
    s32 fontId;
    s32 i;

    if (seqId >= gAudioCtx.numSequences) {
        return NULL;
    }

    fontId = 0xFF;
    index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    numFonts = gAudioCtx.sequenceFontTable[index++];

    while (numFonts > 0) {
        fontId = gAudioCtx.sequenceFontTable[index++];

        font = AudioLoad_SyncLoadFont(fontId);
        numFonts--;
    }

    *outDefaultFontId = fontId;
    return font;
}

void AudioLoad_SyncLoadSeqParts(s32 seqId, s32 arg1) {
    s32 pad;
    u32 defaultFontId;

    if (seqId < gAudioCtx.numSequences) {
        if (arg1 & 2) {
            AudioLoad_SyncLoadSeqFonts(seqId, &defaultFontId);
        }
        if (arg1 & 1) {
            AudioLoad_SyncLoadSeq(seqId);
        }
    }
}

s32 AudioLoad_SyncLoadSample(SoundFontSample* sample, s32 fontId) {
    void* sampleAddr;

    if (sample->unk_bit25 == 1)
    {
        if (sample->medium != MEDIUM_RAM) {
            sampleAddr = AudioHeap_AllocSampleCache(sample->size, fontId, (void*)sample->sampleAddr, sample->medium,
                                                    CACHE_PERSISTENT);
            if (sampleAddr == NULL) {
                return -1;
            }

            if (sample->medium == MEDIUM_UNK) {
                AudioLoad_SyncDmaUnkMedium(sample->sampleAddr, sampleAddr, sample->size,
                                           gAudioCtx.sampleBankTable->unkMediumParam);
            } else {
                AudioLoad_SyncDma(sample->sampleAddr, sampleAddr, sample->size, sample->medium);
            }
            sample->medium = MEDIUM_RAM;
            sample->sampleAddr = sampleAddr;
        }
    }
}

s32 AudioLoad_SyncLoadInstrument(s32 fontId, s32 instId, s32 drumId) {
    if (instId < 0x7F) {
        Instrument* instrument = Audio_GetInstrumentInner(fontId, instId);

        if (instrument == NULL) {
            return -1;
        }
        if (instrument->normalRangeLo != 0) {
            AudioLoad_SyncLoadSample(instrument->lowNotesSound.sample, fontId);
        }
        AudioLoad_SyncLoadSample(instrument->normalNotesSound.sample, fontId);
        if (instrument->normalRangeHi != 0x7F) {
            return AudioLoad_SyncLoadSample(instrument->highNotesSound.sample, fontId);
        }
    } else if (instId == 0x7F) {
        Drum* drum = Audio_GetDrum(fontId, drumId);

        if (drum == NULL) {
            return -1;
        }
        AudioLoad_SyncLoadSample(drum->sound.sample, fontId);
        return 0;
    }
}

void AudioLoad_AsyncLoad(s32 tableType, s32 id, s32 nChunks, s32 retData, OSMesgQueue* retQueue) {
    if (AudioLoad_AsyncLoadInner(tableType, id, nChunks, retData, retQueue) == NULL) {
        osSendMesg32(retQueue, 0xFFFFFFFF, OS_MESG_NOBLOCK);
    }
}

void AudioLoad_AsyncLoadSeq(s32 seqId, s32 arg1, s32 retData, OSMesgQueue* retQueue) {
    AudioLoad_AsyncLoad(SEQUENCE_TABLE, seqId, 0, retData, retQueue);
}

void AudioLoad_AsyncLoadSampleBank(s32 sampleBankId, s32 arg1, s32 retData, OSMesgQueue* retQueue) {
    AudioLoad_AsyncLoad(SAMPLE_TABLE, sampleBankId, 0, retData, retQueue);
}

void AudioLoad_AsyncLoadFont(s32 fontId, s32 arg1, s32 retData, OSMesgQueue* retQueue) {
    AudioLoad_AsyncLoad(FONT_TABLE, fontId, 0, retData, retQueue);
}

u8* AudioLoad_GetFontsForSequence(s32 seqId, u32* outNumFonts) {
    s32 index;

     if (seqId == NA_BGM_DISABLED)
         return NULL;

    u16 newSeqId = AudioEditor_GetReplacementSeq(seqId);
    if (newSeqId > sequenceMapSize || !sequenceMap[newSeqId]) {
        return NULL;
    }
    SequenceData sDat = ResourceMgr_LoadSeqByName(sequenceMap[newSeqId]);

    if (sDat.numFonts == 0)
        return NULL;

    return sDat.fonts;
}

void AudioLoad_DiscardSeqFonts(s32 seqId) {
    s32 fontId;
    s32 index;
    s32 numFonts;

    index = ((u16*)gAudioCtx.sequenceFontTable)[seqId];
    numFonts = gAudioCtx.sequenceFontTable[index++];

    while (numFonts > 0) {
        numFonts--;
        fontId = AudioLoad_GetRealTableIndex(FONT_TABLE, gAudioCtx.sequenceFontTable[index++]);
        if (AudioHeap_SearchPermanentCache(FONT_TABLE, fontId) == NULL) {
            AudioLoad_DiscardFont(fontId);
            AudioLoad_SetFontLoadStatus(fontId, 0);
        }
    }
}

void AudioLoad_DiscardFont(s32 fontId) {
    u32 i;
    AudioCache* pool = &gAudioCtx.fontCache;
    AudioPersistentCache* persistent;

    if (fontId == pool->temporary.entries[0].id) {
        pool->temporary.entries[0].id = -1;
    } else if (fontId == pool->temporary.entries[1].id) {
        pool->temporary.entries[1].id = -1;
    }

    persistent = &pool->persistent;
    for (i = 0; i < persistent->numEntries; i++) {
        if (fontId == persistent->entries[i].id) {
            persistent->entries[i].id = -1;
        }
    }

    AudioHeap_DiscardFont(fontId);
}

s32 AudioLoad_SyncInitSeqPlayer(s32 playerIdx, s32 seqId, s32 arg2) {
    if (gAudioCtx.resetTimer != 0) {
        return 0;
    }

    gAudioCtx.seqPlayers[playerIdx].skipTicks = 0;
    AudioLoad_SyncInitSeqPlayerInternal(playerIdx, seqId, arg2);
    // Intentionally missing return. Returning the result of the above function
    // call matches but is UB because it too is missing a return, and using the
    // result of a non-void function that has failed to return a value is UB.
    // The callers of this function do not use the return value, so it's fine.

    return 1;
}

s32 AudioLoad_SyncInitSeqPlayerSkipTicks(s32 playerIdx, s32 seqId, s32 skipTicks) {
    if (gAudioCtx.resetTimer != 0) {
        return 0;
    }

    gAudioCtx.seqPlayers[playerIdx].skipTicks = skipTicks;
    AudioLoad_SyncInitSeqPlayerInternal(playerIdx, seqId, 0);
    // Missing return, see above.

    return 1;
}

s32 AudioLoad_SyncInitSeqPlayerInternal(s32 playerIdx, s32 seqId, s32 arg2) {
    SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[playerIdx];
    u8* seqData;
    s32 index;
    s32 numFonts;
    s32 fontId;
    s8 authCachePolicy = -1; // since 0 is a valid cache policy value

    AudioSeq_SequencePlayerDisable(seqPlayer);

    fontId = 0xFF;

    if (gAudioCtx.seqReplaced[playerIdx]) {
        authCachePolicy = seqCachePolicyMap[seqId];
        seqId = gAudioCtx.seqToPlay[playerIdx];
    }
    SequenceData seqData2 = ResourceMgr_LoadSeqByName(sequenceMap[seqId]);
    if (authCachePolicy != -1) {
        seqData2.cachePolicy = authCachePolicy;
    }

    for (int i = 0; i < seqData2.numFonts; i++)
    {
        fontId = seqData2.fonts[i];
        AudioLoad_SyncLoadFont(fontId); // NOTE: If this is commented out, then enemies will play child link sounds...
        //numFonts--;
    }

    seqData = AudioLoad_SyncLoadSeq(seqId);

    if (seqData == NULL) {
        return 0;
    }

    AudioSeq_ResetSequencePlayer(seqPlayer);
    seqPlayer->seqId = seqId;
    seqPlayer->defaultFont = fontId;

    seqPlayer->seqData = seqData;
    seqPlayer->enabled = 1;
    seqPlayer->scriptState.pc = seqData;
    seqPlayer->scriptState.depth = 0;
    seqPlayer->delay = 0;
    seqPlayer->finished = 0;
    seqPlayer->playerIdx = playerIdx;

    // Fix for barinade boss fight starting music multiple times
    // this is not noticeable if the sequence is authentic, since the "Boss Battle"
    // sequence begins with some silence
    if (gPlayState != NULL &&
        gPlayState->sceneId == SCENE_JABU_JABU_BOSS &&
        playerIdx == SEQ_PLAYER_BGM_MAIN &&
        seqId != NA_BGM_BOSS) {
        
        seqPlayer->delay = 10;
    }
    
    AudioSeq_SkipForwardSequence(seqPlayer);
    //! @bug missing return (but the return value is not used so it's not UB)
    
    // Keep track of the previous sequence/scene so we don't repeat notifications
    static uint16_t previousSeqId = UINT16_MAX;
    static int16_t previousSceneNum = INT16_MAX;
    if (CVarGetInteger("gSeqNameOverlay", 0) &&
        playerIdx == SEQ_PLAYER_BGM_MAIN &&
        (seqId != previousSeqId || (gPlayState != NULL && gPlayState->sceneId != previousSceneNum))) {
        
        previousSeqId = seqId;
        if (gPlayState != NULL) {
            previousSceneNum = gPlayState->sceneId;
        }
        const char* sequenceName = AudioCollection_GetSequenceName(seqId);
        if (sequenceName != NULL) {
            Overlay_DisplayText_Seconds(CVarGetInteger("gSeqNameOverlayDuration", 5), sequenceName);
        }
    }
}

u8* AudioLoad_SyncLoadSeq(s32 seqId) {
    s32 pad;
    s32 didAllocate;

    if (gAudioCtx.seqLoadStatus[AudioLoad_GetRealTableIndex(SEQUENCE_TABLE, seqId)] == 1) {
        return NULL;
    }

    return AudioLoad_SyncLoad(SEQUENCE_TABLE, seqId, &didAllocate);
}

uintptr_t AudioLoad_GetSampleBank(u32 sampleBankId, u32* outMedium) {
    return AudioLoad_TrySyncLoadSampleBank(sampleBankId, outMedium, true);
}

uintptr_t AudioLoad_TrySyncLoadSampleBank(u32 sampleBankId, u32* outMedium, s32 noLoad) {
    uintptr_t ret;
    AudioTable* sampleBankTable;
    u32 realTableId = AudioLoad_GetRealTableIndex(SAMPLE_TABLE, sampleBankId);
    s8 cachePolicy;

    sampleBankTable = AudioLoad_GetLoadTable(SAMPLE_TABLE);
    ret = AudioLoad_SearchCaches(SAMPLE_TABLE, realTableId);
    if (ret != NULL) {
        if (gAudioCtx.sampleFontLoadStatus[realTableId] != 1) {
            AudioLoad_SetSampleFontLoadStatus(realTableId, 2);
        }
        *outMedium = MEDIUM_RAM;
        return ret;
    }

    //cachePolicy = sampleBankTable->entries[sampleBankId].cachePolicy;
    if (/* cachePolicy == 4 || */ noLoad == true) {
        *outMedium = sampleBankTable->entries[sampleBankId].medium;
        return sampleBankTable->entries[realTableId].romAddr;
    }

    ret = AudioLoad_SyncLoad(SAMPLE_TABLE, sampleBankId, &noLoad);
    if (ret != NULL) {
        *outMedium = MEDIUM_RAM;
        return ret;
    }

    *outMedium = sampleBankTable->entries[sampleBankId].medium;
    return sampleBankTable->entries[realTableId].romAddr;
}

SoundFontData* AudioLoad_SyncLoadFont(u32 fontId) {
    SoundFontData* ret;
    s32 sampleBankId1;
    s32 sampleBankId2;
    s32 didAllocate;
    RelocInfo relocInfo;
    s32 realFontId = AudioLoad_GetRealTableIndex(FONT_TABLE, fontId);

    if (gAudioCtx.fontLoadStatus[realFontId] == 1) {
        return NULL;
    }

    SoundFont* sf = ResourceMgr_LoadAudioSoundFont(fontMap[fontId]);

    sampleBankId1 = sf->sampleBankId1;
    sampleBankId2 = sf->sampleBankId2;

    relocInfo.sampleBankId1 = sampleBankId1;
    relocInfo.sampleBankId2 = sampleBankId2;
    //if (sampleBankId1 != 0xFF) {
        //relocInfo.baseAddr1 = AudioLoad_TrySyncLoadSampleBank(sampleBankId1, &relocInfo.medium1, false);
    //} else {
        relocInfo.baseAddr1 = 0;
    //}

    //if (sampleBankId2 != 0xFF) {
        //relocInfo.baseAddr2 = AudioLoad_TrySyncLoadSampleBank(sampleBankId2, &relocInfo.medium2, false);
    //} else {
        relocInfo.baseAddr2 = 0;
    //}

    ret = AudioLoad_SyncLoad(FONT_TABLE, fontId, &didAllocate);
    if (ret == NULL) {
        return NULL;
    }
    if (didAllocate == true) {
        AudioLoad_RelocateFontAndPreloadSamples(realFontId, ret, &relocInfo, false);
    }

    return ret;
}

uintptr_t AudioLoad_SyncLoad(u32 tableType, u32 id, s32* didAllocate) {
    size_t size;
    AudioTable* table;
    s32 pad;
    u32 medium;
    s32 status;
    uintptr_t romAddr;
    s32 cachePolicy;
    uintptr_t ret;
    u32 realId;

    realId = AudioLoad_GetRealTableIndex(tableType, id);
    ret = AudioLoad_SearchCaches(tableType, realId);
    if (ret != NULL) {
        *didAllocate = false;
        status = 2;
    } else {
        char* seqData = 0;
        SoundFont* fnt;

        if (tableType == SEQUENCE_TABLE)
        {
            SequenceData sData = ResourceMgr_LoadSeqByName(sequenceMap[id]);
            seqData = sData.seqData;
            size = sData.seqDataSize;
            medium = sData.medium;
            cachePolicy = sData.cachePolicy;
            romAddr = 0;
        }
        else if (tableType == FONT_TABLE)
        {
            fnt = ResourceMgr_LoadAudioSoundFont(fontMap[id]);
            size = sizeof(SoundFont);
            medium = 2;
            cachePolicy = 0;
            romAddr = 0;
        }
        else
        {
            //table = AudioLoad_GetLoadTable(tableType);
            //size = table->entries[realId].size;
            //size = ALIGN16(size);
            //medium = table->entries[id].medium;
            //cachePolicy = table->entries[id].cachePolicy;
            //romAddr = table->entries[realId].romAddr;
        }

        switch (cachePolicy) {
            case 0:
                ret = AudioHeap_AllocPermanent(tableType, realId, size);
                if (ret == NULL) {
                    return ret;
                }
                break;
            case 1:
                ret = AudioHeap_AllocCached(tableType, size, CACHE_PERSISTENT, realId);
                if (ret == NULL) {
                    return ret;
                }
                break;
            case 2:
                ret = AudioHeap_AllocCached(tableType, size, CACHE_TEMPORARY, realId);
                if (ret == NULL) {
                    return ret;
                }
                break;
            case 3:
            case 4:
                ret = AudioHeap_AllocCached(tableType, size, CACHE_EITHER, realId);
                if (ret == NULL) {
                    return ret;
                }
                break;
        }

        *didAllocate = true;
        if (medium == MEDIUM_UNK) {
            AudioLoad_SyncDmaUnkMedium(romAddr, ret, size, (s16)table->unkMediumParam);
        } else {
            if (tableType == SEQUENCE_TABLE && seqData != NULL) {
                AudioLoad_SyncDma(seqData, ret, size, medium);
            } else if (tableType == FONT_TABLE) {
                AudioLoad_SyncDma(fnt, ret, size, medium);
            }
            else {
                //AudioLoad_SyncDma(romAddr, ret, size, medium);
            }
        }

        status = cachePolicy == 0 ? 5 : 2;
    }

    switch (tableType) {
        case SEQUENCE_TABLE:
            AudioLoad_SetSeqLoadStatus(realId, status);
            break;
        case FONT_TABLE:
            AudioLoad_SetFontLoadStatus(realId, status);
            break;
        case SAMPLE_TABLE:
            AudioLoad_SetSampleFontLoadStatusAndApplyCaches(realId, status);
            break;
        default:
            break;
    }

    return ret;
}

u32 AudioLoad_GetRealTableIndex(s32 tableType, u32 id)
{
    if ((tableType == SEQUENCE_TABLE || tableType == FONT_TABLE)) {
        return id;
    }

    //AudioTable* table = AudioLoad_GetLoadTable(tableType);

    // If the size is 0, then this entry actually redirects to another entry.
    // The rom address is actually an index into the same table where the "real" data is.
    //if (table->entries[id].size == 0) {
        //id = table->entries[id].romAddr;
    //}

    return id;
}

void* AudioLoad_SearchCaches(s32 tableType, s32 id) {
    void* ret;

    ret = AudioHeap_SearchPermanentCache(tableType, id);
    if (ret != NULL) {
        return ret;
    }

    ret = AudioHeap_SearchCaches(tableType, CACHE_EITHER, id);
    if (ret != NULL) {
        return ret;
    }

    return NULL;
}

AudioTable* AudioLoad_GetLoadTable(s32 tableType) {
    AudioTable* ret;

    switch (tableType) {
        case SEQUENCE_TABLE:
            ret = gAudioCtx.sequenceTable;
            break;
        case FONT_TABLE:
            ret = gAudioCtx.soundFontTable;
            break;
        default:
            ret = NULL;
            break;
        case SAMPLE_TABLE:
            ret = gAudioCtx.sampleBankTable;
            break;
    }
    return ret;
}

void AudioLoad_RelocateFont(s32 fontId, SoundFontData* mem, RelocInfo* relocInfo) {
    uintptr_t reloc;
    uintptr_t reloc2;
    Instrument* inst;
    Drum* drum;
    SoundFontSound* sfx;
    s32 i;
    SoundFont* sf = NULL;
    s32 numDrums = 0;
    s32 numInstruments = 0;
    s32 numSfx = 0;

    sf = ResourceMgr_LoadAudioSoundFont(fontMap[fontId]);
    numDrums = sf->numDrums;
    numInstruments = sf->numInstruments;
    numSfx = sf->numSfx;

    void** ptrs = (void**)mem;

#define BASE_OFFSET(x) (void*)((u32)(x) + (u32)(mem))

    reloc2 = ptrs[0];
    if ((numDrums != 0))
    {
        ptrs[0] = BASE_OFFSET(reloc2);
        for (i = 0; i < numDrums; i++)
        {
            drum = sf->drums[i];

            if (!drum->loaded)
            {
                AudioLoad_RelocateSample(&sf->drums[i]->sound, mem, relocInfo, fontOffsets[fontId]);
                drum->loaded = 1;
            }
        }
    }

    reloc2 = ptrs[1];
    if (numSfx != 0) {
        ptrs[1] = BASE_OFFSET(reloc2);
        for (i = 0; i < numSfx; i++) {
            reloc = (SoundFontSound*)ptrs[1] + i;
            AudioLoad_RelocateSample(&sf->soundEffects[i].sample, mem, relocInfo, fontOffsets[fontId]);
        }
    }

    if (numInstruments > 0x7E) {
        numInstruments = 0x7E;
    }

    int startI = 0;
    int startEC = numInstruments - 1;
    for (i = startI; i <= startEC; i++) {
        ptrs[i] = BASE_OFFSET(ptrs[i]);
        inst = sf->instruments[i];

        if (inst != NULL && !inst->loaded) {
            if (inst->normalRangeLo != 0)
            {
                AudioLoad_RelocateSample(&inst->lowNotesSound, mem, relocInfo, fontOffsets[fontId]);
            }
            AudioLoad_RelocateSample(&inst->normalNotesSound, mem, relocInfo, fontOffsets[fontId]);
            if (inst->normalRangeHi != 0x7F) {
                AudioLoad_RelocateSample(&inst->highNotesSound, mem, relocInfo, fontOffsets[fontId]);
            }

            reloc = inst->envelope;
            inst->loaded = 1;
        }
    }

#undef BASE_OFFSET
}

void AudioLoad_SyncDma(uintptr_t devAddr, u8* addr, size_t size, s32 medium) {
    OSMesgQueue* msgQueue = &gAudioCtx.syncDmaQueue;
    OSIoMesg* ioMesg = &gAudioCtx.syncDmaIoMesg;
    //size = ALIGN16(size);

    Audio_InvalDCache(addr, size);

    while (true) {
        if (size < 0x400) {
            break;
        }
        AudioLoad_Dma(ioMesg, OS_MESG_PRI_HIGH, OS_READ, devAddr, addr, 0x400, msgQueue, medium, "FastCopy");
        osRecvMesg(msgQueue, NULL, OS_MESG_BLOCK);
        size -= 0x400;
        devAddr += 0x400;
        addr += 0x400;
    }

    if (size != 0) {
        AudioLoad_Dma(ioMesg, OS_MESG_PRI_HIGH, OS_READ, devAddr, addr, size, msgQueue, medium, "FastCopy");
        osRecvMesg(msgQueue, NULL, OS_MESG_BLOCK);
    }
}

void AudioLoad_SyncDmaUnkMedium(uintptr_t devAddr, u8* addr, size_t size, s32 unkMediumParam) {
}

s32 AudioLoad_Dma(OSIoMesg* mesg, u32 priority, s32 direction, uintptr_t devAddr, uintptr_t ramAddr, size_t size,
                  OSMesgQueue* reqQueue, s32 medium, const char* dmaFuncType) {
    if (gAudioCtx.resetTimer > 0x10) {
        return -1;
    }

    memcpy(ramAddr, devAddr, size);

    return 0;
}

void AudioLoad_Unused1(void) {
}

void AudioLoad_SyncLoadSimple(u32 tableType, u32 fontId) {
    s32 didAllocate;

    AudioLoad_SyncLoad(tableType, fontId, &didAllocate);
}

void* AudioLoad_AsyncLoadInner(s32 tableType, s32 id, s32 nChunks, s32 retData, OSMesgQueue* retQueue) {
    size_t size;
    AudioTable* sp50;
    void* ret;
    s32 medium;
    s8 cachePolicy;
    uintptr_t devAddr;
    s32 status;
    u32 temp_v0;
    u32 realId;

    switch (tableType) {
        case SEQUENCE_TABLE:
            if (gAudioCtx.seqLoadStatus[realId] == 1) {
                return NULL;
            }
            break;
        case FONT_TABLE:
            if (gAudioCtx.fontLoadStatus[realId] == 1) {
                return NULL;
            }
            break;
        case SAMPLE_TABLE:
            if (gAudioCtx.sampleFontLoadStatus[realId] == 1) {
                return NULL;
            }
            break;
    }

    ret = AudioLoad_SearchCaches(tableType, realId);
    if (ret != NULL) {
        status = 2;
        osSendMesg32(retQueue, MK_ASYNC_MSG(retData, 0, 0, 0), OS_MESG_NOBLOCK);
    } else {
        sp50 = AudioLoad_GetLoadTable(tableType);
        size = sp50->entries[realId].size;
        size = ALIGN16(size);
        medium = sp50->entries[id].medium;
        cachePolicy = sp50->entries[id].cachePolicy;
        devAddr = sp50->entries[realId].romAddr;
        status = 2;
        switch (cachePolicy) {
            case 0:
                ret = AudioHeap_AllocPermanent(tableType, realId, size);
                if (ret == NULL) {
                    return ret;
                }
                status = 5;
                break;
            case 1:
                ret = AudioHeap_AllocCached(tableType, size, CACHE_PERSISTENT, realId);
                if (ret == NULL) {
                    return ret;
                }
                break;
            case 2:
                ret = AudioHeap_AllocCached(tableType, size, CACHE_TEMPORARY, realId);
                if (ret == NULL) {
                    return ret;
                }
                break;
            case 3:
            case 4:
                ret = AudioHeap_AllocCached(tableType, size, CACHE_EITHER, realId);
                if (ret == NULL) {
                    return ret;
                }
                break;
        }

        if (medium == MEDIUM_UNK) {
            AudioLoad_StartAsyncLoadUnkMedium((s16)sp50->unkMediumParam, devAddr, ret, size, medium, nChunks, retQueue,
                                              MK_ASYNC_MSG(retData, tableType, id, status));
        } else {
            AudioLoad_StartAsyncLoad(devAddr, ret, size, medium, nChunks, retQueue,
                                     MK_ASYNC_MSG(retData, tableType, realId, status));
        }
        status = 1;
    }

    switch (tableType) {
        case SEQUENCE_TABLE:
            AudioLoad_SetSeqLoadStatus(realId, status);
            break;
        case FONT_TABLE:
            AudioLoad_SetFontLoadStatus(realId, status);
            break;
        case SAMPLE_TABLE:
            AudioLoad_SetSampleFontLoadStatusAndApplyCaches(realId, status);
            break;
        default:
            break;
    }

    return ret;
}

void AudioLoad_ProcessLoads(s32 resetStatus) {
    AudioLoad_ProcessSlowLoads(resetStatus);
    AudioLoad_ProcessSamplePreloads(resetStatus);
    AudioLoad_ProcessAsyncLoads(resetStatus);
}

void AudioLoad_SetDmaHandler(DmaHandler callback) {
    sDmaHandler = callback;
}

void AudioLoad_SetUnusedHandler(void* callback) {
    sUnusedHandler = callback;
}

void AudioLoad_InitSoundFontMeta(s32 fontId) {
    SoundFont* font = &gAudioCtx.soundFonts[fontId];
    AudioTableEntry* entry = &gAudioCtx.soundFontTable->entries[fontId];

    font->sampleBankId1 = (entry->shortData1 >> 8) & 0xFF;
    font->sampleBankId2 = (entry->shortData1) & 0xFF;
    font->numInstruments = (entry->shortData2 >> 8) & 0xFF;
    font->numDrums = entry->shortData2 & 0xFF;
    font->numSfx = entry->shortData3;
}

s32 AudioLoad_AssertValidAddr(uintptr_t ramAddr, uintptr_t startAddr, size_t size) {
    if (ramAddr >= startAddr && ramAddr < startAddr + size) {
        // Valid
        return 0;
    } else {
        // Not Valid, Breakpoint
        return -1;
    }
}

#define BASE_ROM_OFFSET(x) (uintptr_t)((uintptr_t)(x) + (uintptr_t)(romAddr))

void AudioLoad_InitSwapFontSampleHeaders(SoundFontSample* sample, uintptr_t romAddr) {
}

void AudioLoad_InitSwapFont(void) {
    s32 i;
    s32 fontId;
    s32 numFonts = gAudioCtx.soundFontTable->numEntries;
    AudioTableEntry* fontEntry;
    uintptr_t romAddr;
    size_t size;
    void** ptrs;
    SoundFont* font;
    s32 numDrums;
    s32 numSfxs;
    s32 numInstruments;
    SoundFontSample* sample;
    Drum** drumList;
    Drum* drum;
    SoundFontSound* sfxList;
    SoundFontSound* sfx;
    Instrument** instList;
    Instrument* inst;

    // Only up to (numFonts - 1) as final font has garbage data to prevent corruption and is never used
    for (fontId = 0; fontId < (numFonts - 1); fontId++) {
        // extract meta-data from soundFontTable
        fontEntry = &gAudioCtx.soundFontTable->entries[fontId];
        romAddr = fontEntry->romAddr;
        size = fontEntry->size;
        ptrs = (void**)romAddr;

        // extract meta-data from soundFonts
        font = &gAudioCtx.soundFonts[fontId];
        numDrums = font->numDrums;
        numSfxs = font->numSfx;
        numInstruments = font->numInstruments;

        // drums
        ptrs[0] = (void*)BOMSWAP32((uintptr_t)ptrs[0]);
        if ((ptrs[0] != NULL) && (numDrums != 0)) {
            drumList = (Drum**)BASE_ROM_OFFSET(ptrs[0]);

            for (i = 0; i < numDrums; i++) {
                drumList[i] = (Drum*)BOMSWAP32((uintptr_t)drumList[i]);

                if (drumList[i] != NULL) {
                    drum = (Drum*)BASE_ROM_OFFSET(drumList[i]);

                    sample = (SoundFontSample*)BASE_ROM_OFFSET(drum->sound.sample);
                    AudioLoad_InitSwapFontSampleHeaders(sample, romAddr);
                }
            }
        }

        // sfxs
        ptrs[1] = (void*)BOMSWAP32((uintptr_t)ptrs[1]);
        if ((ptrs[1] != NULL) && (numSfxs != 0)) {
            sfxList = (SoundFontSound*)BASE_ROM_OFFSET(ptrs[1]);

            for (i = 0; i < numSfxs; i++) {

                sfx = &sfxList[i];

                if (sfx->sample != NULL) {
                    SoundFontSample* sample = (SoundFontSample*)BASE_ROM_OFFSET(sfx->sample);
                    AudioLoad_InitSwapFontSampleHeaders(sample, romAddr);
                }
            }
        }

        // instruments
        if (numInstruments > 0x7E) {
            numInstruments = 0x7E;
        }

        instList = (Instrument**)(&ptrs[2]);
        for (i = 0; i < numInstruments; i++) {
            instList[i] = (Instrument*)BOMSWAP32((uintptr_t)instList[i]);

            if (instList[i] != NULL) {
                inst = BASE_ROM_OFFSET(instList[i]);

                if (inst->normalRangeLo != 0) {
                    sample = (SoundFontSample*)BASE_ROM_OFFSET(inst->lowNotesSound.sample);
                    AudioLoad_InitSwapFontSampleHeaders(sample, romAddr);
                }

                sample = (SoundFontSample*)BASE_ROM_OFFSET(inst->normalNotesSound.sample);
                AudioLoad_InitSwapFontSampleHeaders(sample, romAddr);

                if (inst->normalRangeHi != 0x7F) {
                    sample = (SoundFontSample*)BASE_ROM_OFFSET(inst->highNotesSound.sample);
                    AudioLoad_InitSwapFontSampleHeaders(sample, romAddr);
                }
            }
        }
    }

    return;
}

#undef BASE_ROM_OFFSET

int strcmp_sort( const void *str1, const void *str2 ) {
    char *const *pp1 = str1;
    char *const *pp2 = str2;
    return strcmp(*pp1, *pp2);
}

void AudioLoad_Init(void* heap, size_t heapSize) {
    char pad[0x48];
    s32 numFonts;
    void* temp_v0_3;
    s32 i;
    u64* heapP;
    s16* u2974p;

    D_801755D0 = NULL;
    gAudioCtx.resetTimer = 0;

    memset(&gAudioCtx, 0, sizeof(gAudioCtx));
    memset(gAudioCtx.seqToPlay, 0, 8);
    memset(gAudioCtx.seqReplaced, 0, 8);

    switch (osTvType) {
        case OS_TV_PAL:
            gAudioCtx.unk_2960 = 20.03042f;
            gAudioCtx.refreshRate = 50;
            break;
        case OS_TV_MPAL:
            gAudioCtx.unk_2960 = 16.546f;
            gAudioCtx.refreshRate = 60;
            break;
        case OS_TV_NTSC:
        default:
            gAudioCtx.unk_2960 = 16.713f;
            gAudioCtx.refreshRate = 60;
    }

    Audio_InitMesgQueues();

    for (i = 0; i < 3; i++) {
        gAudioCtx.aiBufLengths[i] = 0xA0;
    }

    gAudioCtx.totalTaskCnt = 0;
    gAudioCtx.rspTaskIdx = 0;
    gAudioCtx.curAIBufIdx = 0;
    gAudioCtx.soundMode = 0;
    gAudioCtx.currTask = NULL;
    gAudioCtx.rspTask[0].task.t.data_size = 0;
    gAudioCtx.rspTask[1].task.t.data_size = 0;
    osCreateMesgQueue(&gAudioCtx.syncDmaQueue, &gAudioCtx.syncDmaMesg, 1);
    osCreateMesgQueue(&gAudioCtx.currAudioFrameDmaQueue, gAudioCtx.currAudioFrameDmaMesgBuf, 0x40);
    osCreateMesgQueue(&gAudioCtx.externalLoadQueue, gAudioCtx.externalLoadMesgBuf,
                      ARRAY_COUNT(gAudioCtx.externalLoadMesgBuf));
    osCreateMesgQueue(&gAudioCtx.preloadSampleQueue, gAudioCtx.preloadSampleMesgBuf,
                      ARRAY_COUNT(gAudioCtx.externalLoadMesgBuf));
    gAudioCtx.curAudioFrameDmaCount = 0;
    gAudioCtx.sampleDmaCount = 0;
    gAudioCtx.cartHandle = osCartRomInit();

    if (heap == NULL) {
        gAudioCtx.audioHeap = gAudioHeap;
        gAudioCtx.audioHeapSize = gAudioHeapInitSizes.heapSize;
    } else {
        void** hp = &heap;
        gAudioCtx.audioHeap = *hp;
        gAudioCtx.audioHeapSize = heapSize;
    }

    for (i = 0; i < gAudioCtx.audioHeapSize / 8; i++) {
        ((u64*)gAudioCtx.audioHeap)[i] = 0;
    }

    AudioHeap_InitMainPools(gAudioHeapInitSizes.initPoolSize);

    for (i = 0; i < 3; i++) {
        gAudioCtx.aiBuffers[i] = AudioHeap_AllocZeroed(&gAudioCtx.audioInitPool, AIBUF_LEN * sizeof(s16));
    }

    //gAudioCtx.sequenceTable = (AudioTable*)gSequenceTable;
    //gAudioCtx.soundFontTable = (AudioTable*)gSoundFontTable;
    //gAudioCtx.sampleBankTable = (AudioTable*)gSampleBankTable;
    //gAudioCtx.sequenceFontTable = gSequenceFontTable;
    //gAudioCtx.numSequences = gAudioCtx.sequenceTable->numEntries;

    gAudioCtx.audioResetSpecIdToLoad = 0;
    gAudioCtx.resetStatus = 1;

    AudioHeap_ResetStep();

    int seqListSize = 0;
    int customSeqListSize = 0;
    char** seqList = ResourceMgr_ListFiles("audio/sequences*", &seqListSize);
    char** customSeqList = ResourceMgr_ListFiles("custom/music/*", &customSeqListSize);
    sequenceMapSize = (size_t)(AudioCollection_SequenceMapSize() + customSeqListSize); 
    sequenceMap = malloc(sequenceMapSize * sizeof(char*));
    gAudioCtx.seqLoadStatus = malloc(sequenceMapSize * sizeof(char*));

    for (size_t i = 0; i < seqListSize; i++)
    {
        SequenceData sDat = ResourceMgr_LoadSeqByName(seqList[i]);

        char* str = malloc(strlen(seqList[i]) + 1);
        strcpy(str, seqList[i]);

        sequenceMap[sDat.seqNumber] = str;
        seqCachePolicyMap[sDat.seqNumber] = sDat.cachePolicy;
    }

    free(seqList);

    int startingSeqNum = MAX_AUTHENTIC_SEQID; // 109 is the highest vanilla sequence
    qsort(customSeqList, customSeqListSize, sizeof(char*), strcmp_sort);

    // Because AudioCollection's sequenceMap actually has more than sequences (including instruments from 130-135 and sfx in the 2000s, 6000s, 10000s, 14000s, 18000s, and 26000s),
    // it's better here to keep track of the next empty seqNum in AudioCollection instead of just skipping past the instruments at 130 with a higher MAX_AUTHENTIC_SEQID,
    // especially if those others could be added to in the future. However, this really needs to be streamlined with specific ranges in AudioCollection for types, or unifying
    // AudioCollection and the various maps in here
    int seqNum = startingSeqNum;

    for (size_t i = startingSeqNum; i < startingSeqNum + customSeqListSize; i++) {
        // ensure that what would be the next sequence number is actually unassigned in AudioCollection
        while (AudioCollection_HasSequenceNum(seqNum)) {
            seqNum++;
        }
        int j = i - startingSeqNum;
        AudioCollection_AddToCollection(customSeqList[j], seqNum);
        SequenceData sDat = ResourceMgr_LoadSeqByName(customSeqList[j]);
        sDat.seqNumber = seqNum;

        char* str = malloc(strlen(customSeqList[j]) + 1);
        strcpy(str, customSeqList[j]);

        sequenceMap[sDat.seqNumber] = str;
        seqNum++;
    }

    free(customSeqList);

    int fntListSize = 0;
    char** fntList = ResourceMgr_ListFiles("audio/fonts*", &fntListSize);

    for (int i = 0; i < fntListSize; i++)
    {
        SoundFont* sf = ResourceMgr_LoadAudioSoundFont(fntList[i]);

        char* str = malloc(strlen(fntList[i]) + 1);
        strcpy(str, fntList[i]);

        fontMap[sf->fntIndex] = str;
    }

        numFonts = fntListSize;

        free(fntList);
        gAudioCtx.soundFonts = AudioHeap_Alloc(&gAudioCtx.audioInitPool, numFonts * sizeof(SoundFont));


    if (temp_v0_3 = AudioHeap_Alloc(&gAudioCtx.audioInitPool, gAudioHeapInitSizes.permanentPoolSize), temp_v0_3 == NULL) {
        // cast away const from gAudioHeapInitSizes
        // *((u32*)&gAudioHeapInitSizes.permanentPoolSize) = 0;
    }

    AudioHeap_AllocPoolInit(&gAudioCtx.permanentPool, temp_v0_3, gAudioHeapInitSizes.permanentPoolSize);
    gAudioContextInitialized = true;
    osSendMesg32(gAudioCtx.taskStartQueueP, gAudioCtx.totalTaskCnt, OS_MESG_NOBLOCK);
}

void AudioLoad_InitSlowLoads(void) {
    gAudioCtx.slowLoads[0].status = 0;
    gAudioCtx.slowLoads[1].status = 0;
}

s32 AudioLoad_SlowLoadSample(s32 fontId, s32 instId, s8* isDone) {
    SoundFontSample* sample;
    AudioSlowLoad* slowLoad;

    sample = AudioLoad_GetFontSample(fontId, instId);
    if (sample == NULL) {
        *isDone = 0;
        return -1;
    }

    if (sample->medium == MEDIUM_RAM) {
        *isDone = 2;
        return 0;
    }

    slowLoad = &gAudioCtx.slowLoads[gAudioCtx.slowLoadPos];
    if (slowLoad->status == LOAD_STATUS_DONE) {
        slowLoad->status = LOAD_STATUS_WAITING;
    }

    slowLoad->sample = *sample;
    slowLoad->isDone = isDone;
    slowLoad->curRamAddr =
        AudioHeap_AllocSampleCache(sample->size, fontId, sample->sampleAddr, sample->medium, CACHE_TEMPORARY);

    if (slowLoad->curRamAddr == NULL) {
        if (sample->medium == MEDIUM_UNK || sample->codec == CODEC_S16_INMEMORY) {
            *isDone = 0;
            return -1;
        } else {
            *isDone = 3;
            return -1;
        }
    }

    slowLoad->status = LOAD_STATUS_START;
    slowLoad->bytesRemaining = ALIGN16(sample->size);
    slowLoad->ramAddr = slowLoad->curRamAddr;
    slowLoad->curDevAddr = sample->sampleAddr;
    slowLoad->medium = sample->medium;
    slowLoad->seqOrFontId = fontId;
    slowLoad->instId = instId;
    if (slowLoad->medium == MEDIUM_UNK) {
        slowLoad->unkMediumParam = gAudioCtx.sampleBankTable->unkMediumParam;
    }

    gAudioCtx.slowLoadPos ^= 1;
    return 0;
}

SoundFontSample* AudioLoad_GetFontSample(s32 fontId, s32 instId) {
    SoundFontSample* ret;

    if (instId < 0x80) {
        Instrument* instrument = Audio_GetInstrumentInner(fontId, instId);
        if (instrument == NULL) {
            return NULL;
        }
        ret = instrument->normalNotesSound.sample;
    } else if (instId < 0x100) {
        Drum* drum = Audio_GetDrum(fontId, instId - 0x80);
        if (drum == NULL) {
            return NULL;
        }
        ret = drum->sound.sample;
    } else {
        SoundFontSound* sound = Audio_GetSoundEffect(fontId, instId - 0x100);
        if (sound == NULL) {
            return NULL;
        }
        ret = sound->sample;
    }
    return ret;
}

void AudioLoad_Unused2(void) {
}

void AudioLoad_FinishSlowLoad(AudioSlowLoad* slowLoad) {
    SoundFontSample* sample;

    if (slowLoad->sample.sampleAddr == NULL) {
        return;
    }

    sample = AudioLoad_GetFontSample(slowLoad->seqOrFontId, slowLoad->instId);
    if (sample == NULL) {
        return;
    }

    slowLoad->sample = *sample;
    sample->sampleAddr = slowLoad->ramAddr;
    sample->medium = MEDIUM_RAM;
}

void AudioLoad_ProcessSlowLoads(s32 resetStatus) {
    AudioSlowLoad* slowLoad;
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.slowLoads); i++) {
        slowLoad = &gAudioCtx.slowLoads[i];
        switch (gAudioCtx.slowLoads[i].status) {
            case LOAD_STATUS_LOADING:
                if (slowLoad->medium != MEDIUM_UNK) {
                    osRecvMesg(&slowLoad->msgqueue, NULL, OS_MESG_BLOCK);
                }

                if (resetStatus != 0) {
                    slowLoad->status = LOAD_STATUS_DONE;
                    continue;
                }
            case LOAD_STATUS_START:
                slowLoad->status = LOAD_STATUS_LOADING;
                if (slowLoad->bytesRemaining == 0) {
                    AudioLoad_FinishSlowLoad(slowLoad);
                    slowLoad->status = LOAD_STATUS_DONE;
                    *slowLoad->isDone = 1;
                } else if (slowLoad->bytesRemaining < 0x400) {
                    if (slowLoad->medium == MEDIUM_UNK) {
                        size_t size = slowLoad->bytesRemaining;
                        AudioLoad_DmaSlowCopyUnkMedium(slowLoad->curDevAddr, slowLoad->curRamAddr, size,
                                                       slowLoad->unkMediumParam);
                    } else {
                        AudioLoad_DmaSlowCopy(slowLoad, slowLoad->bytesRemaining);
                    }
                    slowLoad->bytesRemaining = 0;
                } else {
                    if (slowLoad->medium == MEDIUM_UNK) {
                        AudioLoad_DmaSlowCopyUnkMedium(slowLoad->curDevAddr, slowLoad->curRamAddr, 0x400,
                                                       slowLoad->unkMediumParam);
                    } else {
                        AudioLoad_DmaSlowCopy(slowLoad, 0x400);
                    }
                    slowLoad->bytesRemaining -= 0x400;
                    slowLoad->curRamAddr += 0x400;
                    slowLoad->curDevAddr += 0x400;
                }
                break;
        }
    }
}

void AudioLoad_DmaSlowCopy(AudioSlowLoad* slowLoad, size_t size) {
    Audio_InvalDCache(slowLoad->curRamAddr, size);
    osCreateMesgQueue(&slowLoad->msgqueue, &slowLoad->msg, 1);
    AudioLoad_Dma(&slowLoad->ioMesg, OS_MESG_PRI_NORMAL, 0, slowLoad->curDevAddr, slowLoad->curRamAddr, size,
                  &slowLoad->msgqueue, slowLoad->medium, "SLOWCOPY");
}

void AudioLoad_DmaSlowCopyUnkMedium(uintptr_t devAddr, uintptr_t ramAddr, size_t size, s32 arg3) {
}

s32 AudioLoad_SlowLoadSeq(s32 seqId, u8* ramAddr, s8* isDone) {
    AudioSlowLoad* slowLoad;
    AudioTable* seqTable;
    size_t size;

    seqId = AudioLoad_GetRealTableIndex(SEQUENCE_TABLE, seqId);
    u16 newSeqId = AudioEditor_GetReplacementSeq(seqId);
    if (seqId != newSeqId) {
        gAudioCtx.seqToPlay[SEQ_PLAYER_BGM_MAIN] = newSeqId;
        gAudioCtx.seqReplaced[SEQ_PLAYER_BGM_MAIN] = 1;
        // This sequence command starts playing a sequence specified by seqId on the main BGM seq player.
        // The sequence command is a bitpacked u32 where different bits of the number indicated different parameters.
        // What those parameters are is dependent on the first 8 bits which represent an operation.
        // First two digits (bits 31-24) - Sequence Command Operation (0x0 = play sequence immediately)
        // Next two digits (bits 23-16) - Index of the SeqPlayer to operate on. (0, which is the main BGM player.)
        // Next two digits (bits 15-8) - Fade Timer (0 in this case, we don't want any fade-in or out here.)
        // Last two digits (bits 7-0) - the sequence ID to play. Not actually sure why it is cast to u16 instead of u8,
        // copied this from authentic game code and adapted it. I think it might be so that you can choose to encode the
        // fade timer into the seqId if you want to for some reason.
        Audio_QueueSeqCmd(0x00000000 | ((u8)SEQ_PLAYER_BGM_MAIN << 24) | ((u8)(0) << 16) | (u16)seqId);
        return 0;
    }
    seqTable = AudioLoad_GetLoadTable(SEQUENCE_TABLE);
    slowLoad = &gAudioCtx.slowLoads[gAudioCtx.slowLoadPos];
    if (slowLoad->status == LOAD_STATUS_DONE) {
        slowLoad->status = LOAD_STATUS_WAITING;
    }

    slowLoad->sample.sampleAddr = NULL;
    slowLoad->isDone = isDone;

    SequenceData sData = ResourceMgr_LoadSeqByName(sequenceMap[seqId]);
    char* seqData = sData.seqData;
    size = sData.seqDataSize;
    slowLoad->curDevAddr = seqData;
    slowLoad->medium = sData.medium;

    slowLoad->curRamAddr = ramAddr;
    slowLoad->status = LOAD_STATUS_START;
    slowLoad->bytesRemaining = size;
    slowLoad->ramAddr = ramAddr;
    slowLoad->seqOrFontId = seqId;

    if (slowLoad->medium == MEDIUM_UNK) {
        slowLoad->unkMediumParam = seqTable->unkMediumParam;
    }

    gAudioCtx.slowLoadPos ^= 1;
    return 0;
}

void AudioLoad_InitAsyncLoads(void) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.asyncLoads); i++) {
        gAudioCtx.asyncLoads[i].status = 0;
    }
}

AudioAsyncLoad* AudioLoad_StartAsyncLoadUnkMedium(s32 unkMediumParam, uintptr_t devAddr, uintptr_t ramAddr, size_t size,
                                                  s32 medium,
                                                  s32 nChunks, OSMesgQueue* retQueue, s32 retMsg) {
    AudioAsyncLoad* asyncLoad;

    asyncLoad = AudioLoad_StartAsyncLoad(devAddr, ramAddr, size, medium, nChunks, retQueue, retMsg);

    if (asyncLoad == NULL) {
        return NULL;
    }

    osSendMesgPtr(&gAudioCtx.asyncLoadUnkMediumQueue, asyncLoad, OS_MESG_NOBLOCK);
    asyncLoad->unkMediumParam = unkMediumParam;
    return asyncLoad;
}

AudioAsyncLoad* AudioLoad_StartAsyncLoad(uintptr_t devAddr, uintptr_t ramAddr, size_t size, s32 medium, s32 nChunks,
                                         OSMesgQueue* retQueue, s32 retMsg) {
    AudioAsyncLoad* asyncLoad;
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.asyncLoads); i++) {
        if (gAudioCtx.asyncLoads[i].status == 0) {
            asyncLoad = &gAudioCtx.asyncLoads[i];
            break;
        }
    }

    // no more available async loads
    if (i == ARRAY_COUNT(gAudioCtx.asyncLoads)) {
        return NULL;
    }

    asyncLoad->status = LOAD_STATUS_START;
    asyncLoad->curDevAddr = devAddr;
    asyncLoad->ramAddr = ramAddr;
    asyncLoad->curRamAddr = ramAddr;
    asyncLoad->bytesRemaining = size;

    if (nChunks == 0) {
        asyncLoad->chunkSize = 0x1000;
    } else if (nChunks == 1) {
        asyncLoad->chunkSize = size;
    } else {
        asyncLoad->chunkSize = ALIGN256(size / nChunks);
        if (asyncLoad->chunkSize < 0x100) {
            asyncLoad->chunkSize = 0x100;
        }
    }

    asyncLoad->retQueue = retQueue;
    asyncLoad->delay = 3;
    asyncLoad->medium = medium;
    asyncLoad->retMsg = retMsg;
    osCreateMesgQueue(&asyncLoad->msgQueue, &asyncLoad->msg, 1);
    return asyncLoad;
}

void AudioLoad_ProcessAsyncLoads(s32 resetStatus) {
    AudioAsyncLoad* asyncLoad;
    s32 i;

    if (gAudioCtx.resetTimer == 1) {
        return;
    }

    if (gAudioCtx.curUnkMediumLoad == NULL) {
        if (resetStatus != 0) {
            // Clear and ignore queue if resetting.
            do {
            } while (osRecvMesg(&gAudioCtx.asyncLoadUnkMediumQueue, (OSMesg*)&asyncLoad, OS_MESG_NOBLOCK) != -1);
        } else if (osRecvMesg(&gAudioCtx.asyncLoadUnkMediumQueue, (OSMesg*)&asyncLoad, OS_MESG_NOBLOCK) == -1) {
            gAudioCtx.curUnkMediumLoad = NULL;
        } else {
            gAudioCtx.curUnkMediumLoad = asyncLoad;
        }
    }

    if (gAudioCtx.curUnkMediumLoad != NULL) {
        AudioLoad_ProcessAsyncLoadUnkMedium(gAudioCtx.curUnkMediumLoad, resetStatus);
    }

    for (i = 0; i < ARRAY_COUNT(gAudioCtx.asyncLoads); i++) {
        if (gAudioCtx.asyncLoads[i].status == 1) {
            asyncLoad = &gAudioCtx.asyncLoads[i];
            if (asyncLoad->medium != MEDIUM_UNK) {
                AudioLoad_ProcessAsyncLoad(asyncLoad, resetStatus);
            }
        }
    }
}

void AudioLoad_ProcessAsyncLoadUnkMedium(AudioAsyncLoad* asyncLoad, s32 resetStatus) {
}

void AudioLoad_FinishAsyncLoad(AudioAsyncLoad* asyncLoad) {
    u32 retMsg = asyncLoad->retMsg;
    u32 fontId;
    u32 pad;
    OSMesg doneMsg;
    u32 sampleBankId1;
    u32 sampleBankId2;
    RelocInfo relocInfo;

    switch (ASYNC_TBLTYPE(retMsg)) {
        case SEQUENCE_TABLE:
            AudioLoad_SetSeqLoadStatus(ASYNC_ID(retMsg), ASYNC_STATUS(retMsg));
            break;
        case SAMPLE_TABLE:
            AudioLoad_SetSampleFontLoadStatusAndApplyCaches(ASYNC_ID(retMsg), ASYNC_STATUS(retMsg));
            break;
        case FONT_TABLE:
            fontId = ASYNC_ID(retMsg);
            sampleBankId1 = gAudioCtx.soundFonts[fontId].sampleBankId1;
            sampleBankId2 = gAudioCtx.soundFonts[fontId].sampleBankId2;
            relocInfo.sampleBankId1 = sampleBankId1;
            relocInfo.sampleBankId2 = sampleBankId2;
            relocInfo.baseAddr1 =
                sampleBankId1 != 0xFF ? AudioLoad_GetSampleBank(sampleBankId1, &relocInfo.medium1) : 0;
            relocInfo.baseAddr2 =
                sampleBankId2 != 0xFF ? AudioLoad_GetSampleBank(sampleBankId2, &relocInfo.medium2) : 0;
            AudioLoad_SetFontLoadStatus(fontId, ASYNC_STATUS(retMsg));
            AudioLoad_RelocateFontAndPreloadSamples(fontId, asyncLoad->ramAddr, &relocInfo, true);
            break;
    }

    doneMsg.data32 = asyncLoad->retMsg;
    asyncLoad->status = LOAD_STATUS_WAITING;
    osSendMesg(asyncLoad->retQueue, doneMsg, OS_MESG_NOBLOCK);
}

void AudioLoad_ProcessAsyncLoad(AudioAsyncLoad* asyncLoad, s32 resetStatus) {
    AudioTable* sampleBankTable = gAudioCtx.sampleBankTable;

    if (asyncLoad->delay >= 2) {
        asyncLoad->delay--;
        return;
    }

    if (asyncLoad->delay == 1) {
        asyncLoad->delay = 0;
    } else if (resetStatus != 0) {
        // Await the previous DMA response synchronously, then return.
        osRecvMesg(&asyncLoad->msgQueue, NULL, OS_MESG_BLOCK);
        asyncLoad->status = LOAD_STATUS_WAITING;
        return;
    } else if (osRecvMesg(&asyncLoad->msgQueue, NULL, OS_MESG_NOBLOCK) == -1) {
        // If the previous DMA step isn't done, return.
        return;
    }

    if (asyncLoad->bytesRemaining == 0) {
        AudioLoad_FinishAsyncLoad(asyncLoad);
        return;
    }

    if (asyncLoad->bytesRemaining < asyncLoad->chunkSize) {
        if (asyncLoad->medium == MEDIUM_UNK) {
            AudioLoad_AsyncDmaUnkMedium(asyncLoad->curDevAddr, asyncLoad->curRamAddr, asyncLoad->bytesRemaining,
                                        sampleBankTable->unkMediumParam);
        } else {
            AudioLoad_AsyncDma(asyncLoad, asyncLoad->bytesRemaining);
        }
        asyncLoad->bytesRemaining = 0;
        return;
    }

    if (asyncLoad->medium == MEDIUM_UNK) {
        AudioLoad_AsyncDmaUnkMedium(asyncLoad->curDevAddr, asyncLoad->curRamAddr, asyncLoad->chunkSize,
                                    sampleBankTable->unkMediumParam);
    } else {
        AudioLoad_AsyncDma(asyncLoad, asyncLoad->chunkSize);
    }

    asyncLoad->bytesRemaining -= asyncLoad->chunkSize;
    asyncLoad->curDevAddr += asyncLoad->chunkSize;
    asyncLoad->curRamAddr += asyncLoad->chunkSize;
}

void AudioLoad_AsyncDma(AudioAsyncLoad* asyncLoad, size_t size) {
    size = ALIGN16(size);
    Audio_InvalDCache(asyncLoad->curRamAddr, size);
    osCreateMesgQueue(&asyncLoad->msgQueue, &asyncLoad->msg, 1);
    AudioLoad_Dma(&asyncLoad->ioMesg, 0, 0, asyncLoad->curDevAddr, asyncLoad->curRamAddr, size, &asyncLoad->msgQueue,
                  asyncLoad->medium, "BGCOPY");
}

void AudioLoad_AsyncDmaUnkMedium(uintptr_t devAddr, uintptr_t ramAddr, size_t size, s16 arg3) {
}

void AudioLoad_RelocateSample(SoundFontSound* sound, SoundFontData* mem, RelocInfo* relocInfo, int fontId) 
{
}

void AudioLoad_RelocateFontAndPreloadSamples(s32 fontId, SoundFontData* mem, RelocInfo* relocInfo, s32 async) {
    AudioPreloadReq* preload;
    AudioPreloadReq* topPreload;
    SoundFontSample* sample;
    size_t size;
    s32 nChunks;
    u8* addr;
    s32 preloadInProgress;
    s32 i;

    preloadInProgress = false;
    if (gAudioCtx.preloadSampleStackTop != 0) {
        preloadInProgress = true;
    } else {
        D_8016B780 = 0;
    }

    gAudioCtx.numUsedSamples = 0;
    AudioLoad_RelocateFont(fontId, mem, relocInfo);

    size = 0;
    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        size += ALIGN16(gAudioCtx.usedSamples[i]->size);
    }
    if (size && size) {}

    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        if (gAudioCtx.preloadSampleStackTop == 120) {
            break;
        }

        sample = gAudioCtx.usedSamples[i];
        addr = NULL;
        switch (async) {
            case false:
                if (sample->medium == relocInfo->medium1) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId1, sample->sampleAddr,
                                                      sample->medium, CACHE_PERSISTENT);
                } else if (sample->medium == relocInfo->medium2) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId2, sample->sampleAddr,
                                                      sample->medium, CACHE_PERSISTENT);
                } else if (sample->medium == MEDIUM_DISK_DRIVE) {
                    addr = AudioHeap_AllocSampleCache(sample->size, 0xFE, sample->sampleAddr, sample->medium,
                                                      CACHE_PERSISTENT);
                }
                break;

            case true:
                if (sample->medium == relocInfo->medium1) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId1, sample->sampleAddr,
                                                      sample->medium, CACHE_TEMPORARY);
                } else if (sample->medium == relocInfo->medium2) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId2, sample->sampleAddr,
                                                      sample->medium, CACHE_TEMPORARY);
                } else if (sample->medium == MEDIUM_DISK_DRIVE) {
                    addr = AudioHeap_AllocSampleCache(sample->size, 0xFE, sample->sampleAddr, sample->medium,
                                                      CACHE_TEMPORARY);
                }
                break;
        }
        if (addr == NULL) {
            continue;
        }

        switch (async) {
            case false:
                if (sample->medium == MEDIUM_UNK) {
                    AudioLoad_SyncDmaUnkMedium(sample->sampleAddr, addr, sample->size,
                                               gAudioCtx.sampleBankTable->unkMediumParam);
                    sample->sampleAddr = addr;
                    sample->medium = MEDIUM_RAM;
                } else {
                    AudioLoad_SyncDma(sample->sampleAddr, addr, sample->size, sample->medium);
                    sample->sampleAddr = addr;
                    sample->medium = MEDIUM_RAM;
                }
                if (sample->medium == MEDIUM_DISK_DRIVE) {}
                break;

            case true:
                preload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop];
                preload->sample = sample;
                preload->ramAddr = addr;
                preload->encodedInfo = (gAudioCtx.preloadSampleStackTop << 24) | 0xFFFFFF;
                preload->isFree = false;
                preload->endAndMediumKey = sample->sampleAddr + sample->size + sample->medium;
                gAudioCtx.preloadSampleStackTop++;
                break;
        }
    }
    gAudioCtx.numUsedSamples = 0;

    if (gAudioCtx.preloadSampleStackTop != 0 && !preloadInProgress) {
        topPreload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop - 1];
        sample = topPreload->sample;
        nChunks = (sample->size >> 12) + 1;
        AudioLoad_StartAsyncLoad(sample->sampleAddr, topPreload->ramAddr, sample->size, sample->medium, nChunks,
                                 &gAudioCtx.preloadSampleQueue, topPreload->encodedInfo);
    }
}

s32 AudioLoad_ProcessSamplePreloads(s32 resetStatus) {
    SoundFontSample* sample;
    AudioPreloadReq* preload;
    u32 preloadIndex;
    uintptr_t key;
    u32 nChunks;
    s32 pad;

    if (gAudioCtx.preloadSampleStackTop > 0) {
        if (resetStatus != 0) {
            // Clear result queue and preload stack and return.
            osRecvMesg(&gAudioCtx.preloadSampleQueue, (OSMesg*)&preloadIndex, OS_MESG_NOBLOCK);
            gAudioCtx.preloadSampleStackTop = 0;
            return 0;
        }
        if (osRecvMesg(&gAudioCtx.preloadSampleQueue, (OSMesg*)&preloadIndex, OS_MESG_NOBLOCK) == -1) {
            // Previous preload is not done yet.
            return 0;
        }

        preloadIndex >>= 24;
        preload = &gAudioCtx.preloadSampleStack[preloadIndex];

        if (preload->isFree == false) {
            sample = preload->sample;
            key = (u32)sample->sampleAddr + sample->size + sample->medium;
            if (key == preload->endAndMediumKey) {
                // Change storage for sample to the preloaded version.
                sample->sampleAddr = preload->ramAddr;
                sample->medium = MEDIUM_RAM;
            }
            preload->isFree = true;
        }

        // Pop requests with isFree = true off the stack, as far as possible,
        // and dispatch the next DMA.
        for (;;) {
            if (gAudioCtx.preloadSampleStackTop <= 0) {
                break;
            }
            preload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop - 1];
            if (preload->isFree == true) {
                gAudioCtx.preloadSampleStackTop--;
                continue;
            }

            sample = preload->sample;
            nChunks = (sample->size >> 12) + 1;
            key = (u32)sample->sampleAddr + sample->size + sample->medium;
            if (key != preload->endAndMediumKey) {
                preload->isFree = true;
                gAudioCtx.preloadSampleStackTop--;
            } else {
                AudioLoad_StartAsyncLoad((u32)sample->sampleAddr, preload->ramAddr, sample->size, sample->medium,
                                         nChunks, &gAudioCtx.preloadSampleQueue, preload->encodedInfo);
                break;
            }
        }
    }
    return 1;
}

s32 AudioLoad_AddToSampleSet(SoundFontSample* sample, s32 numSamples, SoundFontSample** sampleSet) {
    s32 i;

    for (i = 0; i < numSamples; i++) {
        if (sample->sampleAddr == sampleSet[i]->sampleAddr) {
            break;
        }
    }

    if (i == numSamples) {
        sampleSet[numSamples] = sample;
        numSamples++;
    }

    return numSamples;
}

s32 AudioLoad_GetSamplesForFont(s32 fontId, SoundFontSample** sampleSet) {
    s32 i;
    s32 numDrums;
    s32 numInstruments;
    s32 numSamples = 0;

    numDrums = gAudioCtx.soundFonts[fontId].numDrums;
    numInstruments = gAudioCtx.soundFonts[fontId].numInstruments;

    for (i = 0; i < numDrums; i++) {
        Drum* drum = Audio_GetDrum(fontId, i);
        if (drum != NULL) {
            numSamples = AudioLoad_AddToSampleSet(drum->sound.sample, numSamples, sampleSet);
        }
    }

    for (i = 0; i < numInstruments; i++) {
        Instrument* instrument = Audio_GetInstrumentInner(fontId, i);
        if (instrument != NULL) {
            if (instrument->normalRangeLo != 0) {
                numSamples = AudioLoad_AddToSampleSet(instrument->lowNotesSound.sample, numSamples, sampleSet);
            }
            if (instrument->normalRangeHi != 0x7F) {
                numSamples = AudioLoad_AddToSampleSet(instrument->highNotesSound.sample, numSamples, sampleSet);
            }
            numSamples = AudioLoad_AddToSampleSet(instrument->normalNotesSound.sample, numSamples, sampleSet);
        }
    }

    // Should really also process sfx, but this method is never called, so whatever.
    return numSamples;
}

void AudioLoad_AddUsedSample(SoundFontSound* sound) {
    SoundFontSample* sample = sound->sample;

    if (sample != NULL) {
        if ((sample->size != 0) && (sample->unk_bit26) && (sample->medium != MEDIUM_RAM)) {
            gAudioCtx.usedSamples[gAudioCtx.numUsedSamples++] = sample;
        }
    }
}

void AudioLoad_PreloadSamplesForFont(s32 fontId, s32 async, RelocInfo* relocInfo) {
    s32 numDrums;
    s32 numInstruments;
    s32 numSfx;
    Drum* drum;
    Instrument* instrument;
    SoundFontSound* sound;
    AudioPreloadReq* preload;
    AudioPreloadReq* topPreload;
    u8* addr = NULL;
    size_t size;
    s32 i;
    SoundFontSample* sample;
    s32 preloadInProgress;
    s32 nChunks;

    preloadInProgress = false;
    if (gAudioCtx.preloadSampleStackTop != 0) {
        preloadInProgress = true;
    }

    gAudioCtx.numUsedSamples = 0;

    SoundFont* sf = ResourceMgr_LoadAudioSoundFont(fontMap[fontId]);

    numDrums = sf->numDrums;
    numInstruments = sf->numInstruments;
    numSfx = sf->numSfx;

    for (i = 0; i < numInstruments; i++) {
        instrument = Audio_GetInstrumentInner(fontId, i);
        if (instrument != NULL) {
            if (instrument->normalRangeLo != 0) {
                AudioLoad_AddUsedSample(&instrument->lowNotesSound);
            }
            if (instrument->normalRangeHi != 0x7F) {
                AudioLoad_AddUsedSample(&instrument->highNotesSound);
            }
            AudioLoad_AddUsedSample(&instrument->normalNotesSound);
        }
    }

    for (i = 0; i < numDrums; i++) {
        drum = Audio_GetDrum(fontId, i);
        if (drum != NULL) {
            AudioLoad_AddUsedSample(&drum->sound);
        }
    }

    for (i = 0; i < numSfx; i++) {
        sound = Audio_GetSoundEffect(fontId, i);
        if (sound != NULL) {
            AudioLoad_AddUsedSample(sound);
        }
    }

    if (gAudioCtx.numUsedSamples == 0) {
        return;
    }

    size = 0;
    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        size += ALIGN16(gAudioCtx.usedSamples[i]->size);
    }
    if (size) {}

    for (i = 0; i < gAudioCtx.numUsedSamples; i++) {
        if (gAudioCtx.preloadSampleStackTop == 120) {
            break;
        }

        sample = gAudioCtx.usedSamples[i];
        if (sample->medium == MEDIUM_RAM) {
            continue;
        }

        switch (async) {
            case false:
                if (sample->medium == relocInfo->medium1) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId1, sample->sampleAddr,
                                                      sample->medium, CACHE_PERSISTENT);
                } else if (sample->medium == relocInfo->medium2) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId2, sample->sampleAddr,
                                                      sample->medium, CACHE_PERSISTENT);
                }
                break;

            case true:
                if (sample->medium == relocInfo->medium1) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId1, sample->sampleAddr,
                                                      sample->medium, CACHE_TEMPORARY);
                } else if (sample->medium == relocInfo->medium2) {
                    addr = AudioHeap_AllocSampleCache(sample->size, relocInfo->sampleBankId2, sample->sampleAddr,
                                                      sample->medium, CACHE_TEMPORARY);
                }
                break;
        }
        if (addr == NULL) {
            continue;
        }

        switch (async) {
            case false:
                if (sample->medium == MEDIUM_UNK) {
                    AudioLoad_SyncDmaUnkMedium(sample->sampleAddr, addr, sample->size,
                                               gAudioCtx.sampleBankTable->unkMediumParam);
                    sample->sampleAddr = addr;
                    sample->medium = MEDIUM_RAM;
                } else {
                    AudioLoad_SyncDma(sample->sampleAddr, addr, sample->size, sample->medium);
                    sample->sampleAddr = addr;
                    sample->medium = MEDIUM_RAM;
                }
                break;

            case true:
                preload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop];
                preload->sample = sample;
                preload->ramAddr = addr;
                preload->encodedInfo = (gAudioCtx.preloadSampleStackTop << 24) | 0xFFFFFF;
                preload->isFree = false;
                preload->endAndMediumKey = sample->sampleAddr + sample->size + sample->medium;
                gAudioCtx.preloadSampleStackTop++;
                break;
        }
    }
    gAudioCtx.numUsedSamples = 0;

    if (gAudioCtx.preloadSampleStackTop != 0 && !preloadInProgress) {
        topPreload = &gAudioCtx.preloadSampleStack[gAudioCtx.preloadSampleStackTop - 1];
        sample = topPreload->sample;
        nChunks = (sample->size >> 12) + 1;
        AudioLoad_StartAsyncLoad(sample->sampleAddr, topPreload->ramAddr, sample->size, sample->medium, nChunks,
                                 &gAudioCtx.preloadSampleQueue, topPreload->encodedInfo);
    }
}

void AudioLoad_LoadPermanentSamples(void) {
    s32 pad;
    u32 fontId;
    AudioTable* sampleBankTable;
    s32 pad2;
    s32 i;

    sampleBankTable = AudioLoad_GetLoadTable(SAMPLE_TABLE);
    for (i = 0; i < gAudioCtx.permanentPool.count; i++) {
        RelocInfo relocInfo;

        if (gAudioCtx.permanentCache[i].tableType == FONT_TABLE)
        {
            fontId = AudioLoad_GetRealTableIndex(FONT_TABLE, gAudioCtx.permanentCache[i].id);
            //fontId = gAudioCtx.permanentCache[i].id;

            SoundFont* sf = ResourceMgr_LoadAudioSoundFont(fontMap[fontId]);
            relocInfo.sampleBankId1 = sf->sampleBankId1;
            relocInfo.sampleBankId2 = sf->sampleBankId2;

            /*
            if (relocInfo.sampleBankId1 != 0xFF) {
                relocInfo.sampleBankId1 = AudioLoad_GetRealTableIndex(SAMPLE_TABLE, relocInfo.sampleBankId1);
                relocInfo.medium1 = sampleBankTable->entries[relocInfo.sampleBankId1].medium;
            }

            if (relocInfo.sampleBankId2 != 0xFF) {
                relocInfo.sampleBankId2 = AudioLoad_GetRealTableIndex(SAMPLE_TABLE, relocInfo.sampleBankId2);
                relocInfo.medium2 = sampleBankTable->entries[relocInfo.sampleBankId2].medium;
            }
            */

            AudioLoad_PreloadSamplesForFont(fontId, false, &relocInfo);
        }
    }
}

void AudioLoad_Unused3(void) {
}

void AudioLoad_Unused4(void) {
}

void AudioLoad_Unused5(void) {
}

void AudioLoad_ScriptLoad(s32 tableType, s32 id, s8* isDone) {
    static u32 sLoadIndex = 0;
    sScriptLoadDonePointers[sLoadIndex] = isDone;
    AudioLoad_AsyncLoad(tableType, id, 0, sLoadIndex, &sScriptLoadQueue);
    sLoadIndex++;
    if (sLoadIndex == 0x10) {
        sLoadIndex = 0;
    }
}

void AudioLoad_ProcessScriptLoads(void) {
    u32 temp;
    u32 sp20;
    s8* isDone;

    if (osRecvMesg(&sScriptLoadQueue, (OSMesg*)&sp20, OS_MESG_NOBLOCK) != -1) {
        temp = sp20 >> 24;
        isDone = sScriptLoadDonePointers[temp];
        if (isDone != NULL) {
            *isDone = 0;
        }
    }
}

void AudioLoad_InitScriptLoads(void) {
    osCreateMesgQueue(&sScriptLoadQueue, sScriptLoadMesgBuf, ARRAY_COUNT(sScriptLoadMesgBuf));
}
