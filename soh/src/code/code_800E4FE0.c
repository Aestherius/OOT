#include "global.h"

#define SAMPLES_TO_OVERPRODUCE 0x10
#define EXTRA_BUFFERED_AI_SAMPLES_TARGET 0x80

typedef enum {
    CHAN_UPD_UNK_0,           // 0
    CHAN_UPD_VOL_SCALE,       // 1
    CHAN_UPD_VOL,             // 2
    CHAN_UPD_PAN_SIGNED,      // 3
    CHAN_UPD_FREQ_SCALE,      // 4
    CHAN_UPD_REVERB,          // 5
    CHAN_UPD_SCRIPT_IO,       // 6
    CHAN_UPD_PAN_UNSIGNED,    // 7
    CHAN_UPD_STOP_SOMETHING2, // 8
    CHAN_UPD_MUTE_BEHAVE,     // 9
    CHAN_UPD_VIBE_X8,         // 10
    CHAN_UPD_VIBE_X32,        // 11
    CHAN_UPD_UNK_0F,          // 12
    CHAN_UPD_UNK_20,          // 13
    CHAN_UPD_STEREO           // 14
} ChannelUpdateType;

void func_800E6300(SequenceChannel* channel, AudioCmd* arg1);
void func_800E59AC(s32 playerIdx, s32 fadeTimer);
void Audio_InitMesgQueues(void);
AudioTask* func_800E5000(void);
void Audio_ProcessCmds(u32);
void func_800E6128(SequencePlayer* seqPlayer, AudioCmd* arg1);
void func_800E5958(s32 playerIdx, s32 fadeTimer);
s32 func_800E66C0(s32 arg0);

// AudioMgr_Retrace
AudioTask* func_800E4FE0(void) {
    return func_800E5000();
}

extern u64 rspAspMainDataStart[];
extern u64 rspAspMainDataEnd[];

void AudioMgr_CreateNextAudioBuffer(s16* samples, u32 num_samples) {
    OSMesg sp4C;

    gAudioCtx.totalTaskCnt++;

    AudioLoad_DecreaseSampleDmaTtls();
    AudioLoad_ProcessLoads(gAudioCtx.resetStatus);
    AudioLoad_ProcessScriptLoads();

    if (gAudioCtx.resetStatus != 0) {
        if (AudioHeap_ResetStep() == 0) {
            if (gAudioCtx.resetStatus == 0) {
                osSendMesg8(gAudioCtx.audioResetQueueP, gAudioCtx.audioResetSpecIdToLoad, OS_MESG_NOBLOCK);
            }
        }
    }

    int j = 0;
    if (gAudioCtx.resetStatus == 0) {
        // msg = 0000RREE R = read pos, E = End Pos
        while (osRecvMesg(gAudioCtx.cmdProcQueueP, &sp4C, OS_MESG_NOBLOCK) != -1) {
            Audio_ProcessCmds(sp4C.data32);
            j++;
        }
        if ((j == 0) && (gAudioCtx.cmdQueueFinished)) {
            Audio_ScheduleProcessCmds();
        }
    }
    s32 writtenCmds;
    AudioSynth_Update(gAudioCtx.curAbiCmdBuf, &writtenCmds, samples, num_samples);
    gAudioCtx.audioRandom = (gAudioCtx.audioRandom + gAudioCtx.totalTaskCnt) * osGetCount();
}

AudioTask* func_800E5000(void) {
    return NULL;

    static s32 sMaxAbiCmdCnt = 0x80;
    static AudioTask* sWaitingAudioTask = NULL;
    u32 samplesRemainingInAi;
    s32 abiCmdCnt;
    s32 pad;
    s32 j;
    s32 sp5C;
    s16* currAiBuffer;
    OSTask_t* task;
    s32 index;
    OSMesg sp4C;
    s32 sp48;
    s32 i;

    gAudioCtx.totalTaskCnt++;
    if (gAudioCtx.totalTaskCnt % (gAudioCtx.audioBufferParameters.specUnk4) != 0) {
        if (D_801755D0 != NULL) {
            D_801755D0();
        }

        if ((gAudioCtx.totalTaskCnt % gAudioCtx.audioBufferParameters.specUnk4) + 1 ==
            gAudioCtx.audioBufferParameters.specUnk4) {
            return sWaitingAudioTask;
        } else {
            return NULL;
        }
    }

    osSendMesg32(gAudioCtx.taskStartQueueP, gAudioCtx.totalTaskCnt, OS_MESG_NOBLOCK);
    gAudioCtx.rspTaskIdx ^= 1;
    gAudioCtx.curAIBufIdx++;
    gAudioCtx.curAIBufIdx %= 3;
    index = (gAudioCtx.curAIBufIdx - 2 + 3) % 3;
    samplesRemainingInAi = osAiGetLength() / 4;

    if (gAudioCtx.resetTimer < 16) {
        if (gAudioCtx.aiBufLengths[index] != 0) {
            osAiSetNextBuffer(gAudioCtx.aiBuffers[index], gAudioCtx.aiBufLengths[index] * 4);
            if (gAudioCtx.aiBuffers[index]) {}
            if (gAudioCtx.aiBufLengths[index]) {}
        }
    }

    if (D_801755D0 != NULL) {
        D_801755D0();
    }

    sp5C = gAudioCtx.curAudioFrameDmaCount;
    for (i = 0; i < gAudioCtx.curAudioFrameDmaCount; i++) {
        if (osRecvMesg(&gAudioCtx.currAudioFrameDmaQueue, NULL, OS_MESG_NOBLOCK) == 0) {
            sp5C--;
        }
    }

    if (sp5C != 0) {
        for (i = 0; i < sp5C; i++) {
            osRecvMesg(&gAudioCtx.currAudioFrameDmaQueue, NULL, OS_MESG_BLOCK);
        }
    }

    sp48 = gAudioCtx.currAudioFrameDmaQueue.validCount;
    if (sp48 != 0) {
        for (i = 0; i < sp48; i++) {
            osRecvMesg(&gAudioCtx.currAudioFrameDmaQueue, NULL, OS_MESG_NOBLOCK);
        }
    }

    gAudioCtx.curAudioFrameDmaCount = 0;
    AudioLoad_DecreaseSampleDmaTtls();
    AudioLoad_ProcessLoads(gAudioCtx.resetStatus);
    AudioLoad_ProcessScriptLoads();

    if (gAudioCtx.resetStatus != 0) {
        if (AudioHeap_ResetStep() == 0) {
            if (gAudioCtx.resetStatus == 0) {
                osSendMesg8(gAudioCtx.audioResetQueueP, gAudioCtx.audioResetSpecIdToLoad, OS_MESG_NOBLOCK);
            }

            sWaitingAudioTask = NULL;
            return NULL;
        }
    }

    if (gAudioCtx.resetTimer > 16) {
        return NULL;
    }
    if (gAudioCtx.resetTimer != 0) {
        gAudioCtx.resetTimer++;
    }

    gAudioCtx.currTask = &gAudioCtx.rspTask[gAudioCtx.rspTaskIdx];
    gAudioCtx.curAbiCmdBuf = gAudioCtx.abiCmdBufs[gAudioCtx.rspTaskIdx];

    index = gAudioCtx.curAIBufIdx;
    currAiBuffer = gAudioCtx.aiBuffers[index];

    gAudioCtx.aiBufLengths[index] =
        (s16)((((gAudioCtx.audioBufferParameters.samplesPerFrameTarget - samplesRemainingInAi) +
                EXTRA_BUFFERED_AI_SAMPLES_TARGET) &
               ~0xF) +
              SAMPLES_TO_OVERPRODUCE);
    if (gAudioCtx.aiBufLengths[index] < gAudioCtx.audioBufferParameters.minAiBufferLength) {
        gAudioCtx.aiBufLengths[index] = gAudioCtx.audioBufferParameters.minAiBufferLength;
    }

    if (gAudioCtx.aiBufLengths[index] > gAudioCtx.audioBufferParameters.maxAiBufferLength) {
        gAudioCtx.aiBufLengths[index] = gAudioCtx.audioBufferParameters.maxAiBufferLength;
    }

    j = 0;
    if (gAudioCtx.resetStatus == 0) {
        // msg = 0000RREE R = read pos, E = End Pos
        while (osRecvMesg(gAudioCtx.cmdProcQueueP, &sp4C, OS_MESG_NOBLOCK) != -1) {
            Audio_ProcessCmds(sp4C.data32);
            j++;
        }
        if ((j == 0) && (gAudioCtx.cmdQueueFinished)) {
            Audio_ScheduleProcessCmds();
        }
    }

    gAudioCtx.curAbiCmdBuf =
        AudioSynth_Update(gAudioCtx.curAbiCmdBuf, &abiCmdCnt, currAiBuffer, gAudioCtx.aiBufLengths[index]);
    gAudioCtx.audioRandom = (gAudioCtx.audioRandom + gAudioCtx.totalTaskCnt) * osGetCount();
    gAudioCtx.audioRandom =
        gAudioCtx.aiBuffers[index][gAudioCtx.totalTaskCnt & 0xFF] + gAudioCtx.audioRandom;
    gWaveSamples[8] = (s16*)(((u8*)func_800E4FE0) + (gAudioCtx.audioRandom & 0xFFF0));

    index = gAudioCtx.rspTaskIdx;
    gAudioCtx.currTask->taskQueue = NULL;
    gAudioCtx.currTask->unk_44 = NULL;

    task = &gAudioCtx.currTask->task.t;
    task->type = M_AUDTASK;
    task->flags = 0;
    task->ucode_boot_size = 0x1000;
    task->ucode_size = 0x1000;
    task->dram_stack = NULL;
    task->dram_stack_size = 0;
    task->output_buff = NULL;
    task->output_buff_size = NULL;
    task->data_ptr = (u64*)gAudioCtx.abiCmdBufs[index];
    task->data_size = abiCmdCnt * sizeof(Acmd);
    task->yield_data_ptr = NULL;
    task->yield_data_size = 0;

    if (sMaxAbiCmdCnt < abiCmdCnt) {
        sMaxAbiCmdCnt = abiCmdCnt;
    }

    if (gAudioCtx.audioBufferParameters.specUnk4 == 1) {
        return gAudioCtx.currTask;
    } else {
        sWaitingAudioTask = gAudioCtx.currTask;
        return NULL;
    }
}

#define ACMD_SND_MDE ((u32)0xF0000000)
#define ACMD_MUTE ((u32)0xF1000000)

void func_800E5584(AudioCmd* cmd) {
    s32 i;
    s32 pad;
    s32 pad2;
    u32 temp_a1_5;
    u32 temp_t7;

    switch (cmd->op) {
        case 0x81:
            AudioLoad_SyncLoadSeqParts(cmd->arg1, cmd->arg2);
            return;
        case 0x82:
            AudioLoad_SyncInitSeqPlayer(cmd->arg0, cmd->arg1, cmd->arg2);
            func_800E59AC(cmd->arg0, cmd->data);
            return;
        case 0x85:
            AudioLoad_SyncInitSeqPlayerSkipTicks(cmd->arg0, cmd->arg1, cmd->data);
            return;
        case 0x83:
            if (gAudioCtx.seqPlayers[cmd->arg0].enabled) {
                if (cmd->asInt == 0) {
                    AudioSeq_SequencePlayerDisableAsFinished(&gAudioCtx.seqPlayers[cmd->arg0]);
                } else {
                    func_800E5958(cmd->arg0, cmd->asInt);
                }
            }
            return;
        case 0xF0:
            gAudioCtx.soundMode = cmd->asUInt;
            return;
        case 0xF1:
            for (i = 0; i < gAudioCtx.audioBufferParameters.numSequencePlayers; i++) {
                SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[i];
                seqPlayer->muted = 1;
                seqPlayer->recalculateVolume = 1;
            }
            return;
        case 0xF2:
            if (cmd->asUInt == 1) {
                for (i = 0; i < gAudioCtx.numNotes; i++) {
                    Note* note = &gAudioCtx.notes[i];
                    NoteSubEu* subEu = &note->noteSubEu;
                    if (subEu->bitField0.enabled && note->playbackState.unk_04 == 0) {
                        if (note->playbackState.parentLayer->channel->muteBehavior & 8) {
                            subEu->bitField0.finished = 1;
                        }
                    }
                }
            }

            for (i = 0; i < gAudioCtx.audioBufferParameters.numSequencePlayers; i++) {
                SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[i];
                seqPlayer->muted = 0;
                seqPlayer->recalculateVolume = 1;
            }

            return;
        case 0xF3:
            AudioLoad_SyncLoadInstrument(cmd->arg0, cmd->arg1, cmd->arg2);
            return;
        case 0xF4:
            AudioLoad_AsyncLoadSampleBank(cmd->arg0, cmd->arg1, cmd->arg2, &gAudioCtx.externalLoadQueue);
            return;
        case 0xF5:
            AudioLoad_AsyncLoadFont(cmd->arg0, cmd->arg1, cmd->arg2, &gAudioCtx.externalLoadQueue);
            return;
        case 0xFC:
            AudioLoad_AsyncLoadSeq(cmd->arg0, cmd->arg1, cmd->arg2, &gAudioCtx.externalLoadQueue);
            return;
        case 0xF6:
            AudioLoad_DiscardSeqFonts(cmd->arg1);
            return;
        case 0x90:
            gAudioCtx.unk_5BDC[cmd->arg0] = cmd->asUShort;
            return;
        case 0xF9:
            gAudioCtx.resetStatus = 5;
            gAudioCtx.audioResetSpecIdToLoad = cmd->asUInt;
            return;
        case 0xFB:
            D_801755D0 = (void (*)(void))cmd->asUInt;
            return;
        case 0xE0:
        case 0xE1:
        case 0xE2:
            Audio_SetFontInstrument(cmd->op - 0xE0, cmd->arg0, cmd->arg1, cmd->data);
            return;
        case 0xFE:
            temp_t7 = cmd->asUInt;
            if (temp_t7 == 1) {
                for (i = 0; i < gAudioCtx.audioBufferParameters.numSequencePlayers; i++) {
                    SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[i];
                    if (seqPlayer->enabled) {
                        AudioSeq_SequencePlayerDisableAsFinished(seqPlayer);
                    }
                }
            }
            func_800E66C0(temp_t7);
            return;
        case 0xE3:
            AudioHeap_PopCache(cmd->asInt);
            return;
        default:
            return;
    }
}

// SetFadeOutTimer
void func_800E5958(s32 playerIdx, s32 fadeTimer) {
    SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[playerIdx];

    if (fadeTimer == 0) {
        fadeTimer = 1;
    }

    seqPlayer->fadeVelocity = -(seqPlayer->fadeVolume / fadeTimer);
    seqPlayer->state = 2;
    seqPlayer->fadeTimer = fadeTimer;
}

// SetFadeInTimer
void func_800E59AC(s32 playerIdx, s32 fadeTimer) {
    SequencePlayer* seqPlayer;
    if (fadeTimer != 0) {
        seqPlayer = &gAudioCtx.seqPlayers[playerIdx];
        seqPlayer->state = 1;
        seqPlayer->fadeTimerUnkEu = fadeTimer;
        seqPlayer->fadeTimer = fadeTimer;
        seqPlayer->fadeVolume = 0.0f;
        seqPlayer->fadeVelocity = 0.0f;
    }
}

void Audio_InitMesgQueuesInternal(void) {
    gAudioCtx.cmdWrPos = 0;
    gAudioCtx.cmdRdPos = 0;
    gAudioCtx.cmdQueueFinished = 0;
    gAudioCtx.taskStartQueueP = &gAudioCtx.taskStartQueue;
    gAudioCtx.cmdProcQueueP = &gAudioCtx.cmdProcQueue;
    gAudioCtx.audioResetQueueP = &gAudioCtx.audioResetQueue;
    osCreateMesgQueue(gAudioCtx.taskStartQueueP, gAudioCtx.taskStartMsgs,
                      ARRAY_COUNT(gAudioCtx.taskStartMsgs));
    osCreateMesgQueue(gAudioCtx.cmdProcQueueP, gAudioCtx.cmdProcMsgs, ARRAY_COUNT(gAudioCtx.cmdProcMsgs));
    osCreateMesgQueue(gAudioCtx.audioResetQueueP, gAudioCtx.audioResetMesgs,
                      ARRAY_COUNT(gAudioCtx.audioResetMesgs));
}

void Audio_QueueCmd(u32 opArgs, u32 data) {
    AudioCmd* cmd = &gAudioCtx.cmdBuf[gAudioCtx.cmdWrPos & 0xFF];

    cmd->opArgs = opArgs;
    cmd->data = data;

    gAudioCtx.cmdWrPos++;

    if (gAudioCtx.cmdWrPos == gAudioCtx.cmdRdPos) {
        gAudioCtx.cmdWrPos--;
    }
}

void Audio_QueueCmdF32(u32 opArgs, f32 data) {
    union
    {
        f32 f;
        u32 u;
    } uData = { .f = data };
    Audio_QueueCmd(opArgs, uData.u);
}

void Audio_QueueCmdS32(u32 opArgs, s32 data) {
    Audio_QueueCmd(opArgs, data);
}

void Audio_QueueCmdS8(u32 opArgs, s8 data) {
    u32 uData = data << 0x18;

    Audio_QueueCmd(opArgs, uData);
}

void Audio_QueueCmdU16(u32 opArgs, u16 data) {
    u32 uData = data << 0x10;

    Audio_QueueCmd(opArgs, uData);
}

s32 Audio_ScheduleProcessCmds(void) {
    static s32 D_801304E8 = 0;
    s32 ret;

    if (D_801304E8 < (u8)((gAudioCtx.cmdWrPos - gAudioCtx.cmdRdPos) + 0x100)) {
        D_801304E8 = (u8)((gAudioCtx.cmdWrPos - gAudioCtx.cmdRdPos) + 0x100);
    }

    ret =
        osSendMesg32(gAudioCtx.cmdProcQueueP,
                   (((gAudioCtx.cmdRdPos & 0xFF) << 8) | (gAudioCtx.cmdWrPos & 0xFF)), OS_MESG_NOBLOCK);
    if (ret != -1) {
        gAudioCtx.cmdRdPos = gAudioCtx.cmdWrPos;
        ret = 0;
    } else {
        return -1;
    }

    return ret;
}

void Audio_ResetCmdQueue(void) {
    gAudioCtx.cmdQueueFinished = 0;
    gAudioCtx.cmdRdPos = gAudioCtx.cmdWrPos;
}

void Audio_ProcessCmd(AudioCmd* cmd) {
    SequencePlayer* seqPlayer;
    u16 phi_v0;
    s32 i;

    if ((cmd->op & 0xF0) == 0xF0) {
        func_800E5584(cmd);
        return;
    }

    if (cmd->arg0 < gAudioCtx.audioBufferParameters.numSequencePlayers) {
        seqPlayer = &gAudioCtx.seqPlayers[cmd->arg0];
        if (cmd->op & 0x80) {
            func_800E5584(cmd);
            return;
        }
        if (cmd->op & 0x40) {
            func_800E6128(seqPlayer, cmd);
            return;
        }

        if (cmd->arg1 < 0x10) {
            func_800E6300(seqPlayer->channels[cmd->arg1], cmd);
            return;
        }
        if (cmd->arg1 == 0xFF) {
            phi_v0 = gAudioCtx.unk_5BDC[cmd->arg0];
            for (i = 0; i < 0x10; i++) {
                if (phi_v0 & 1) {
                    func_800E6300(seqPlayer->channels[i], cmd);
                }
                phi_v0 = phi_v0 >> 1;
            }
        }
    }
}

void Audio_ProcessCmds(u32 msg) {
    static u8 curCmdRdPos = 0;
    AudioCmd* cmd;
    u8 endPos;

    if (!gAudioCtx.cmdQueueFinished) {
        curCmdRdPos = msg >> 8;
    }

    while (true) {
        endPos = msg & 0xFF;
        if (curCmdRdPos == endPos) {
            gAudioCtx.cmdQueueFinished = 0;
            return;
        }

        cmd = &gAudioCtx.cmdBuf[curCmdRdPos++ & 0xFF];
        if (cmd->op == 0xF8) {
            gAudioCtx.cmdQueueFinished = 1;
            return;
        }

        Audio_ProcessCmd(cmd);
        cmd->op = 0;
    }
}

u32 func_800E5E20(u32* out) {
    u32 sp1C;

    if (osRecvMesg(&gAudioCtx.externalLoadQueue, (OSMesg*)&sp1C, OS_MESG_NOBLOCK) == -1) {
        *out = 0;
        return 0;
    }
    *out = sp1C & 0xFFFFFF;
    return sp1C >> 0x18;
}

u8* func_800E5E84(s32 arg0, u32* arg1) {
    return AudioLoad_GetFontsForSequence(arg0, arg1);
}

void func_800E5EA4(s32 arg0, u32* arg1, u32* arg2) {
    *arg1 = gAudioCtx.soundFonts[arg0].sampleBankId1;
    *arg2 = gAudioCtx.soundFonts[arg0].sampleBankId2;
}

s32 func_800E5EDC(void) {
    s32 pad;
    OSMesg sp18;

    if (osRecvMesg(gAudioCtx.audioResetQueueP, &sp18, OS_MESG_NOBLOCK) == -1) {
        return 0;
    } else if (gAudioCtx.audioResetSpecIdToLoad != sp18.data8) {
        return -1;
    } else {
        return 1;
    }
}

void func_800E5F34(void) {
    // macro?
    // clang-format off
    s32 chk = -1; OSMesg sp28; do {} while (osRecvMesg(gAudioCtx.audioResetQueueP, &sp28, OS_MESG_NOBLOCK) != chk);
    // clang-format on
}

s32 func_800E5F88(s32 resetPreloadID) {
    s32 resetStatus;
    OSMesg msg;
    s32 pad;

    func_800E5F34();
    resetStatus = gAudioCtx.resetStatus;
    if (resetStatus != 0) {
        Audio_ResetCmdQueue();
        if (gAudioCtx.audioResetSpecIdToLoad == resetPreloadID) {
            return -2;
        } else if (resetStatus > 2) {
            gAudioCtx.audioResetSpecIdToLoad = resetPreloadID;
            return -3;
        } else {
            osRecvMesg(gAudioCtx.audioResetQueueP, &msg, OS_MESG_BLOCK);
        }
    }

    func_800E5F34();
    Audio_QueueCmdS32(0xF9000000, resetPreloadID);

    return Audio_ScheduleProcessCmds();
}

void Audio_PreNMIInternal(void) {
    gAudioCtx.resetTimer = 1;
    if (gAudioContextInitialized) {
        func_800E5F88(0);
        gAudioCtx.resetStatus = 0;
    }
}

s8 func_800E6070(s32 playerIdx, s32 channelIdx, s32 scriptIdx) {
    SequencePlayer* seqPlayer = &gAudioCtx.seqPlayers[playerIdx];
    SequenceChannel* channel;
    if (seqPlayer->enabled) {
        channel = seqPlayer->channels[channelIdx];
        return channel->soundScriptIO[scriptIdx];
    } else {
        return -1;
    }
}

s8 func_800E60C4(s32 playerIdx, s32 arg1) {
    return gAudioCtx.seqPlayers[playerIdx].soundScriptIO[arg1];
}

void Audio_InitExternalPool(void* mem, size_t size) {
    AudioHeap_AllocPoolInit(&gAudioCtx.externalPool, mem, size);
}

void Audio_DestroyExternalPool(void) {
    gAudioCtx.externalPool.start = NULL;
}

void Audio_SetGameVolume(int player_id, f32 volume) {
    gAudioCtx.seqPlayers[player_id].gameVolume = volume;
    gAudioCtx.seqPlayers[player_id].recalculateVolume = true;
}

float Audio_GetGameVolume(int player_id) {
    return gAudioCtx.seqPlayers[player_id].gameVolume;
}

void func_800E6128(SequencePlayer* seqPlayer, AudioCmd* cmd) {
    f32 fadeVolume;
    switch (cmd->op) {
        case 0x41:
            if (seqPlayer->fadeVolumeScale != cmd->asFloat) {
                seqPlayer->fadeVolumeScale = cmd->asFloat;
                seqPlayer->recalculateVolume = 1;
            }
            return;
        case 0x47:
            seqPlayer->tempo = cmd->asInt * 0x30;
            return;
        case 0x49:
            seqPlayer->unk_0C = cmd->asInt * 0x30;
            return;
        case 0x4E:
            seqPlayer->unk_0C = cmd->asInt;
            return;
        case 0x48:
            seqPlayer->transposition = cmd->asSbyte;
            return;
        case 0x46:
            seqPlayer->soundScriptIO[cmd->arg2] = cmd->asSbyte;
            return;
        case 0x4A:
            fadeVolume = (s32)cmd->arg1 / 127.0f;
            goto block_11;
        case 0x4B:
            fadeVolume = ((s32)cmd->arg1 / 100.0f) * seqPlayer->fadeVolume;
        block_11:
            if (seqPlayer->state != 2) {
                seqPlayer->volume = seqPlayer->fadeVolume;
                if (cmd->asInt == 0) {
                    seqPlayer->fadeVolume = fadeVolume;
                } else {
                    s32 tmp = cmd->asInt;
                    seqPlayer->state = 0;
                    seqPlayer->fadeTimer = tmp;
                    seqPlayer->fadeVelocity = (fadeVolume - seqPlayer->fadeVolume) / tmp;
                }
            }
            return;
        case 0x4C:
            if (seqPlayer->state != 2) {
                if (cmd->asInt == 0) {
                    seqPlayer->fadeVolume = seqPlayer->volume;
                } else {
                    s32 tmp = cmd->asInt;
                    seqPlayer->state = 0;
                    seqPlayer->fadeTimer = tmp;
                    seqPlayer->fadeVelocity = (seqPlayer->volume - seqPlayer->fadeVolume) / tmp;
                }
            }
            return;
        case 0x4D:
            seqPlayer->unk_34 = cmd->asFloat;
            if (seqPlayer->unk_34 == 1.0f) {
                seqPlayer->unk_0b1 = 0;
            } else {
                seqPlayer->unk_0b1 = 1;
            }
    }
}

void func_800E6300(SequenceChannel* channel, AudioCmd* cmd) {
    switch (cmd->op) {
        case CHAN_UPD_VOL_SCALE:
            if (channel->volumeScale != cmd->asFloat) {
                channel->volumeScale = cmd->asFloat;
                channel->changes.s.volume = 1;
            }
            return;
        case CHAN_UPD_VOL:
            if (channel->volume != cmd->asFloat) {
                channel->volume = cmd->asFloat;
                channel->changes.s.volume = 1;
            }
            return;
        case CHAN_UPD_PAN_SIGNED:
            if (channel->newPan != cmd->asSbyte) {
                channel->newPan = cmd->asSbyte;
                channel->changes.s.pan = 1;
            }
            return;
        case CHAN_UPD_PAN_UNSIGNED:
            if (channel->newPan != cmd->asSbyte) {
                channel->panChannelWeight = cmd->asSbyte;
                channel->changes.s.pan = 1;
            }
            return;
        case CHAN_UPD_FREQ_SCALE:
            if (channel->freqScale != cmd->asFloat) {
                channel->freqScale = cmd->asFloat;
                channel->changes.s.freqScale = 1;
            }
            return;
        case CHAN_UPD_REVERB:
            if (channel->reverb != cmd->asSbyte) {
                channel->reverb = cmd->asSbyte;
            }
            return;
        case CHAN_UPD_SCRIPT_IO:
            if (cmd->arg2 < 8) {
                channel->soundScriptIO[cmd->arg2] = cmd->asSbyte;
            }
            return;
        case CHAN_UPD_STOP_SOMETHING2:
            channel->stopSomething2 = cmd->asSbyte;
            return;
        case CHAN_UPD_MUTE_BEHAVE:
            channel->muteBehavior = cmd->asSbyte;
            return;
        case CHAN_UPD_VIBE_X8:
            channel->vibratoExtentTarget = cmd->asUbyte * 8;
            channel->vibratoExtentChangeDelay = 1;
            return;
        case CHAN_UPD_VIBE_X32:
            channel->vibratoRateTarget = cmd->asUbyte * 32;
            channel->vibratoRateChangeDelay = 1;
            return;
        case CHAN_UPD_UNK_0F:
            channel->unk_0F = cmd->asUbyte;
            return;
        case CHAN_UPD_UNK_20:
            channel->unk_20 = cmd->asUShort;
            return;
        case CHAN_UPD_STEREO:
            channel->stereo.asByte = cmd->asUbyte;
            return;
    }
}

void func_800E64B0(s32 arg0, s32 arg1, s32 arg2) {
    Audio_QueueCmdS32(((arg0 & 0xFF) << 0x10) | 0xFA000000 | ((arg1 & 0xFF) << 8) | (arg2 & 0xFF), 1);
}

void func_800E64F8(void) {
    Audio_QueueCmdS32(0xFA000000, 0);
}

void func_800E651C(u32 arg0, s32 arg1) {
    Audio_QueueCmdS32((arg1 & 0xFF) | 0xFD000000, arg0);
}

void Audio_WaitForAudioTask(void) {
    osRecvMesg(gAudioCtx.taskStartQueueP, NULL, OS_MESG_NOBLOCK);
    osRecvMesg(gAudioCtx.taskStartQueueP, NULL, OS_MESG_BLOCK);
}

s32 func_800E6590(s32 playerIdx, s32 arg1, s32 arg2) {
    SequencePlayer* seqPlayer;
    SequenceLayer* layer;
    Note* note;
    SoundFontSound* sound;
    s32 loopEnd;
    s32 samplePos;

    seqPlayer = &gAudioCtx.seqPlayers[playerIdx];
    if (seqPlayer->enabled && seqPlayer->channels[arg1]->enabled) {
        layer = seqPlayer->channels[arg1]->layers[arg2];
        if (layer == NULL) {
            return 0;
        }

        if (layer->enabled) {
            if (layer->note == NULL) {
                return 0;
            }

            if (!layer->bit3) {
                return 0;
            }

            note = layer->note;
            if (layer == note->playbackState.parentLayer) {
                sound = note->noteSubEu.sound.soundFontSound;
                if (sound == NULL) {
                    return 0;
                }
                loopEnd = sound->sample->loop->end;
                samplePos = note->synthesisState.samplePosInt;
                return loopEnd - samplePos;
            }
            return 0;
        }
    }
    return 0;
}

s32 func_800E6680(void) {
    return func_800E66C0(0);
}

void func_800E66A0(void) {
    func_800E66C0(2);
}

s32 func_800E66C0(s32 arg0) {
    s32 phi_v1;
    NotePlaybackState* temp_a2;
    NoteSubEu* temp_a3;
    s32 i;
    Note* note;
    SoundFontSound* sound;

    phi_v1 = 0;
    for (i = 0; i < gAudioCtx.numNotes; i++) {
        note = &gAudioCtx.notes[i];
        temp_a2 = &note->playbackState;
        if (note->noteSubEu.bitField0.enabled) {
            temp_a3 = &note->noteSubEu;
            if (temp_a2->adsr.action.s.state != 0) {
                if (arg0 >= 2) {
                    sound = temp_a3->sound.soundFontSound;
                    if (sound == NULL || temp_a3->bitField1.isSyntheticWave) {
                        continue;
                    }
                    if (sound->sample->medium == MEDIUM_RAM) {
                        continue;
                    }
                }

                phi_v1++;
                if ((arg0 & 1) == 1) {
                    temp_a2->adsr.fadeOutVel = gAudioCtx.audioBufferParameters.updatesPerFrameInv;
                    temp_a2->adsr.action.s.release = 1;
                }
            }
        }
    }
    return phi_v1;
}

u32 Audio_NextRandom(void) {
    static u32 audRand = 0x12345678;

    audRand = ((osGetCount() + 0x1234567) * (audRand + gAudioCtx.totalTaskCnt));
    audRand += gAudioCtx.audioRandom;
    return audRand;
}

void Audio_InitMesgQueues(void) {
    Audio_InitMesgQueuesInternal();
}
