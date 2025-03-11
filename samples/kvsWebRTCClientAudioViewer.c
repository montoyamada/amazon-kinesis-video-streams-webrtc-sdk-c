/******************************************************************************
 *  File: kvsWebRTCClientAudioViewer.c
 *
 *  説明:
 *    KVS WebRTC Viewer として起動し、受信音声(Opus)をデコードして
 *    stdout 経由で出力するサンプルコード。
 *    シェル上でパイプを使って aplay に渡せばリアルタイム再生できる。
 *    例: 
 *      ./kvsWebRTCClientAudioViewer MyChannel opus | \
 *          aplay -f S16_LE -r 16000 -c 1 -t raw
 *
 ******************************************************************************/

#include "Samples.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <opus/opus.h>

// PCM再生用パラメータ定義
#define SAMPLE_RATE         16000
#define CHANNELS            1
#define BYTES_PER_SAMPLE    2
#define FRAME_DURATION_MS   20
#define SAMPLES_PER_FRAME   ((SAMPLE_RATE * FRAME_DURATION_MS) / 1000)  // 320 samples
#define FRAME_SIZE          (SAMPLES_PER_FRAME * BYTES_PER_SAMPLE)      // 640 bytes
#define PCM_BUFFER_SIZE     (SAMPLE_RATE * BYTES_PER_SAMPLE * 5)        // 5秒分
#define MAX_FRAME_SAMPLES   5760  // Opus の最大フレームサイズ(サンプル/チャンネル)

// PCMリングバッファ構造体
typedef struct {
    uint8_t data[PCM_BUFFER_SIZE];
    size_t write_index;
    size_t read_index;
    size_t fill_level;  // 現在格納されているバイト数
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PCMBuffer;

// グローバル変数：PCMバッファおよび再生スレッド停止制御用フラグ
static PCMBuffer g_pcmBuffer;
static volatile int g_running = 1;

extern PSampleConfiguration gSampleConfiguration;

#ifdef ENABLE_DATA_CHANNEL
VOID dataChannelOnMessageCallback(UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(pDataChannel);
    if (isBinary) {
        DLOGI("DataChannel Binary Message");
    } else {
        DLOGI("DataChannel String Message: %.*s", pMessageLen, pMessage);
    }
}

VOID dataChannelOnOpenCallback(UINT64 customData, PRtcDataChannel pDataChannel)
{
    STATUS retStatus = STATUS_SUCCESS;
    DLOGI("New DataChannel has been opened %s ", pDataChannel->name);
    dataChannelOnMessage(pDataChannel, customData, dataChannelOnMessageCallback);
    ATOMIC_INCREMENT((PSIZE_T) customData);
    retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) VIEWER_DATA_CHANNEL_MESSAGE, STRLEN(VIEWER_DATA_CHANNEL_MESSAGE));
    if (retStatus != STATUS_SUCCESS) {
        DLOGI("[KVS Viewer] dataChannelSend(): operation returned status code: 0x%08x ", retStatus);
    }
}
#endif // ENABLE_DATA_CHANNEL

// ===== [ADDED] 適応型ジッターバッファ用の構造体とコード追加 start =====

// 受信フレームをジッターバッファに格納する際のラッパー構造体
typedef struct {
    Frame frame;
    UINT64 arrivalTs; // 受信時刻(システム時刻: getEpochTimestampInHundredsOfNanos 等)
} JitterFrame;

// 適応型ジッターバッファ管理構造体
#define MAX_JITTER_FRAMES  128  // 簡易的にフレームを保持する数
typedef struct {
    JitterFrame frames[MAX_JITTER_FRAMES];
    UINT32 head;
    UINT32 tail;
    UINT32 count;

    pthread_mutex_t mutex;
    pthread_cond_t cond;

    // バッファ遅延(ミリ秒)の目標値
    INT32 targetDelayMs;
    INT32 minDelayMs;
    INT32 maxDelayMs;

    // 前フレーム関連(適応制御用)
    UINT64 prevArrivalTs;
    UINT64 prevFramePts;  // 前フレームのpresentationTs
} AdaptiveJitterBuffer;

// ジッターバッファインスタンス
static AdaptiveJitterBuffer g_jitterBuffer;
static pthread_t g_jitterThread; // ジッターバッファ管理スレッド

// ジッターバッファ初期化
void initAdaptiveJitterBuffer(AdaptiveJitterBuffer* jb, INT32 initialDelayMs, INT32 minDelayMs, INT32 maxDelayMs)
{
    memset(jb, 0, sizeof(AdaptiveJitterBuffer));
    jb->targetDelayMs = initialDelayMs;
    jb->minDelayMs = minDelayMs;
    jb->maxDelayMs = maxDelayMs;
    pthread_mutex_init(&jb->mutex, NULL);
    pthread_cond_init(&jb->cond, NULL);
}

// ジッターバッファにフレームを押し込む
void pushJitterFrame(AdaptiveJitterBuffer* jb, const Frame* pFrame, UINT64 arrivalTs)
{
    pthread_mutex_lock(&jb->mutex);

    // いっぱいなら古いフレームを破棄 (tail を進める)
    if (jb->count >= MAX_JITTER_FRAMES) {
        jb->tail = (jb->tail + 1) % MAX_JITTER_FRAMES;
        jb->count--;
    }

    // 末尾に格納
    UINT32 pos = (jb->head) % MAX_JITTER_FRAMES;
    jb->frames[pos].frame = *pFrame; // shallow copy (frameDataもコピー元はSDK管理)
    jb->frames[pos].arrivalTs = arrivalTs;
    jb->head = (jb->head + 1) % MAX_JITTER_FRAMES;
    jb->count++;

    // 遅延適応制御(簡易): 前フレームとの到着間隔差から targetDelayMs を微調整
    if (jb->prevArrivalTs != 0) {
        // フレーム間PTS差 (単位: 100ns → ms換算)
        INT64 ptsDiffMs = (pFrame->presentationTs - jb->prevFramePts) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        // 実際の到着時間差
        INT64 arrDiffMs = (arrivalTs - jb->prevArrivalTs) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

        // 差分(実際 - 理想)を適当なスケールで取り入れる (非常に簡易)
        INT64 diff = arrDiffMs - ptsDiffMs;
        jb->targetDelayMs += diff / 10; // 過剰反応を防ぐために割り算

        // クリップ
        if (jb->targetDelayMs < jb->minDelayMs) {
            jb->targetDelayMs = jb->minDelayMs;
        } else if (jb->targetDelayMs > jb->maxDelayMs) {
            jb->targetDelayMs = jb->maxDelayMs;
        }
    }
    jb->prevArrivalTs = arrivalTs;
    jb->prevFramePts = pFrame->presentationTs;

    pthread_cond_signal(&jb->cond);
    pthread_mutex_unlock(&jb->mutex);
}

// ジッターバッファから「再生時刻が来た」フレームを取得(なければ待機)
// 戻り値: フレームを取得できたらTRUE, タイムアウト等で取得不可ならFALSE
BOOL popJitterFrame(AdaptiveJitterBuffer* jb, Frame* pOutFrame)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1; // 1秒タイムアウト (適宜調整)

    pthread_mutex_lock(&jb->mutex);

    while (jb->count == 0) {
        if (pthread_cond_timedwait(&jb->cond, &jb->mutex, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&jb->mutex);
            return FALSE;
        }
    }

    // バッファ先頭のフレームをチェック
    UINT32 pos = jb->tail;
    JitterFrame jf = jb->frames[pos];

    // 現在時刻
    UINT64 now = GETTIME(); // KVS提供の100ns単位 API (または自前実装)
    // フレームの理想再生時刻 = presentationTs + targetDelayMs
    //   presentationTs (100ns単位) → ms換算して加算 → 100nsに戻す
    UINT64 idealPlayback = jf.frame.presentationTs +
        (UINT64) jb->targetDelayMs * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

    // まだ再生時刻に達していない場合は待機
    if (now < idealPlayback) {
        // cond_wait の再待機
        INT64 diffNs = (idealPlayback - now);
        // 大きすぎる待機は上限クリップ
        if (diffNs > 500LL * HUNDREDS_OF_NANOS_IN_A_MILLISECOND) {
            diffNs = 500LL * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        }
        struct timespec waitTime;
        clock_gettime(CLOCK_REALTIME, &waitTime);
        // 100ns → ns
        long addSec = diffNs / (10000000LL); // 1s = 1000ms = 10000000 * 100ns
        long addNsec = (diffNs % 10000000LL) * 100;

        waitTime.tv_sec += addSec;
        long nsecTmp = waitTime.tv_nsec + addNsec;
        if (nsecTmp >= 1000000000L) {
            waitTime.tv_sec += 1;
            nsecTmp -= 1000000000L;
        }
        waitTime.tv_nsec = nsecTmp;

        pthread_cond_timedwait(&jb->cond, &jb->mutex, &waitTime);

        // タイムアウトや新フレーム到着で再度チェック
        // ここで「すでに再生時刻を過ぎていればすぐ再生」とする
        now = GETTIME();
        if (now < idealPlayback) {
            // まだ早い場合は「もう1度スキップ」などのロジックを入れても良いが、
            // サンプルではこれ以上待たずに再生する。
        }
    }

    // ここでは先頭フレームを返す
    *pOutFrame = jf.frame;
    jb->tail = (jb->tail + 1) % MAX_JITTER_FRAMES;
    jb->count--;

    pthread_mutex_unlock(&jb->mutex);
    return TRUE;
}

// Opusデコード(16kHz/mono)する関数 (既存の例より切り出し)
int decodeOpusFrame(const uint8_t* opusData, size_t opusSize, int16_t* pcmOut, size_t* pcmOutSize);

// ジッターバッファ管理スレッド：
//   1) popJitterFrame (適切な時刻まで待機)
//   2) Opusデコード
//   3) リングバッファにプッシュ
void* jitterBufferThreadFn(void* arg)
{
    (void)arg;
    FILE* outFile = stdout; // ログ用（必要に応じて）

    while (g_running) {
        Frame frame;
        if (!popJitterFrame(&g_jitterBuffer, &frame)) {
            // タイムアウトや待機解除
            continue;
        }

        // 取り出せたフレームをデコード
        int16_t pcmData[SAMPLES_PER_FRAME];
        size_t pcmDataSize = 0;
        if (decodeOpusFrame(frame.frameData, frame.size, pcmData, &pcmDataSize) != 0) {
            DLOGE("Opus decoding failed");
            continue;
        }

        // デコード結果を既存のPCMリングバッファへ格納
        pushPCMData(&g_pcmBuffer, (uint8_t*)pcmData, pcmDataSize);
    }

    return NULL;
}

// ===== [ADDED] 適応型ジッターバッファ用の構造体とコード追加 end =====

// PCMバッファ初期化
void initPCMBuffer(PCMBuffer* buf) {
    memset(buf->data, 0, PCM_BUFFER_SIZE);
    buf->write_index = 0;
    buf->read_index = 0;
    buf->fill_level = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->cond, NULL);
}

// PCMデータをバッファにプッシュする（余裕がない場合は古いフレームを捨てる）
void pushPCMData(PCMBuffer* buf, uint8_t* data, size_t len) {
    pthread_mutex_lock(&buf->mutex);

    while (buf->fill_level + len > PCM_BUFFER_SIZE) {
        buf->read_index = (buf->read_index + FRAME_SIZE) % PCM_BUFFER_SIZE;
        if (buf->fill_level >= FRAME_SIZE) {
            buf->fill_level -= FRAME_SIZE;
        } else {
            buf->fill_level = 0;
        }
    }

    size_t space_to_end = PCM_BUFFER_SIZE - buf->write_index;
    if (len <= space_to_end) {
        memcpy(&buf->data[buf->write_index], data, len);
        buf->write_index = (buf->write_index + len) % PCM_BUFFER_SIZE;
    } else {
        memcpy(&buf->data[buf->write_index], data, space_to_end);
        memcpy(&buf->data[0], data + space_to_end, len - space_to_end);
        buf->write_index = len - space_to_end;
    }

    buf->fill_level += len;
    pthread_cond_signal(&buf->cond);
    pthread_mutex_unlock(&buf->mutex);
}

// バッファからPCMデータをポップする
size_t popPCMData(PCMBuffer* buf, uint8_t* out, size_t len) {
    pthread_mutex_lock(&buf->mutex);
    size_t bytes_available = buf->fill_level;
    size_t bytes_to_read = (bytes_available >= len) ? len : bytes_available;

    if (bytes_to_read > 0) {
        size_t space_to_end = PCM_BUFFER_SIZE - buf->read_index;
        if (bytes_to_read <= space_to_end) {
            memcpy(out, &buf->data[buf->read_index], bytes_to_read);
            buf->read_index = (buf->read_index + bytes_to_read) % PCM_BUFFER_SIZE;
        } else {
            memcpy(out, &buf->data[buf->read_index], space_to_end);
            memcpy(out + space_to_end, &buf->data[0], bytes_to_read - space_to_end);
            buf->read_index = bytes_to_read - space_to_end;
        }
        buf->fill_level -= bytes_to_read;
    }

    pthread_mutex_unlock(&buf->mutex);
    return bytes_to_read;
}

// ===== [CHANGED] 受信コールバック: フレームを直接デコードせずにジッターバッファへ =====
VOID sampleAudioFrameHandler3(UINT64 customData, PFrame pFrame)
{
    UNUSED_PARAM(customData);
    DLOGV("Audio Frame received. TrackId: %" PRIu64 ", Size: %u, Flags %u",
          pFrame->trackId, pFrame->size, pFrame->flags);

    // 受信時刻(システム時刻)を取得 (KVSのGETTIME()など100ns単位関数を利用)
    UINT64 arrivalTs = GETTIME();

    // ここではフレームをジッターバッファへ放り込むだけ
    pushJitterFrame(&g_jitterBuffer, pFrame, arrivalTs);
}
// ===== [END CHANGED] =====

// Opus フレームを 16kHz/mono PCM にデコードする (既存の実装を関数化)
int decodeOpusFrame(const uint8_t* opusData, size_t opusSize, int16_t* pcmOut, size_t* pcmOutSize) {
    static OpusDecoder *decoder = NULL;
    static int decoder_channels = 0;
    int error;

    if (!decoder) {
        decoder = opus_decoder_create(48000, 2, &error);
        if (error != OPUS_OK) {
            fprintf(stderr, "Failed to create Opus decoder: %s\n", opus_strerror(error));
            return error;
        }
        decoder_channels = 2;
    }

    int16_t decoded[MAX_FRAME_SAMPLES * decoder_channels];
    int samples_per_channel = opus_decode(decoder, opusData, opusSize,
                                          decoded, MAX_FRAME_SAMPLES, 0);
    if (samples_per_channel < 0) {
        fprintf(stderr, "Opus decoding error: %s\n", opus_strerror(samples_per_channel));
        return samples_per_channel;
    }

    // モノラルにダウンミックス
    int16_t mono[MAX_FRAME_SAMPLES];
    for (int i = 0; i < samples_per_channel; i++) {
        if (decoder_channels == 1) {
            mono[i] = decoded[i];
        } else {
            int sum = 0;
            for (int ch = 0; ch < decoder_channels; ch++) {
                sum += decoded[i * decoder_channels + ch];
            }
            mono[i] = (int16_t)(sum / decoder_channels);
        }
    }

    // 48000Hz -> 16000Hz (1/3)
    int target_samples = samples_per_channel / 3;
    if (target_samples > SAMPLES_PER_FRAME) {
        target_samples = SAMPLES_PER_FRAME;
    }
    for (int i = 0; i < target_samples; i++) {
        pcmOut[i] = mono[i * 3];
    }
    *pcmOutSize = target_samples * sizeof(int16_t);

    return 0;
}

// 再生スレッド：バッファから PCM データを取り出し、標準出力へ書き出す
void* playbackThread(void* arg) {
    (void)arg;

    FILE* outFile = stdout;
    uint8_t frameBuffer[FRAME_SIZE];
    uint8_t silentFrame[FRAME_SIZE];
    memset(silentFrame, 0, FRAME_SIZE);

    while (g_running) {
        size_t bytesRead = popPCMData(&g_pcmBuffer, frameBuffer, FRAME_SIZE);
        if (bytesRead < FRAME_SIZE) {
            if (bytesRead > 0) {
                fwrite(frameBuffer, 1, bytesRead, outFile);
            }
            fwrite(silentFrame, 1, FRAME_SIZE - bytesRead, outFile);
        } else {
            fwrite(frameBuffer, 1, FRAME_SIZE, outFile);
        }
        fflush(outFile);

        // 1フレーム分の再生タイミング（20ms）に合わせてスリープ
        usleep(FRAME_DURATION_MS * 1000);
    }

    return NULL;
}

#ifndef _WIN32
static void sigintHandler_here(int signum)
{
    UNUSED_PARAM(signum);
    ATOMIC_STORE_BOOL(&gSampleConfiguration->interrupted, TRUE);
    g_running = 0;
}
#endif

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit offerSessionDescriptionInit;
    UINT32 buffLen = 0;
    SignalingMessage message;
    PSampleConfiguration pSampleConfiguration = NULL;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    BOOL locked = FALSE;
    PCHAR pChannelName;
    CHAR clientId[256];

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();

#ifndef _WIN32
    signal(SIGINT, sigintHandler_here);
#endif

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = argc > 1 ? argv[1] : GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif

    if (argc > 2) {
        if (!STRCMP(argv[2], AUDIO_CODEC_NAME_OPUS)) {
            audioCodec = RTC_CODEC_OPUS;
        } else if (!STRCMP(argv[2], AUDIO_CODEC_NAME_ALAW)) {
            audioCodec = RTC_CODEC_ALAW;
        } else if (!STRCMP(argv[2], AUDIO_CODEC_NAME_MULAW)) {
            audioCodec = RTC_CODEC_MULAW;
        } else {
            DLOGI("[KVS Viewer] Defaulting to Opus audio codec");
        }
    }

    if (argc > 3) {
        if (!STRCMP(argv[3], VIDEO_CODEC_NAME_H265)) {
            videoCodec = RTC_CODEC_H265;
        } else if (!STRCMP(argv[3], VIDEO_CODEC_NAME_VP8)) {
            videoCodec = RTC_CODEC_VP8;
        } else {
            DLOGI("[KVS Viewer] Defaulting to H264 video codec");
        }
    }

    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_VIEWER, TRUE, TRUE, logLevel, &pSampleConfiguration));
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    pSampleConfiguration->audioCodec = audioCodec;
    pSampleConfiguration->videoCodec = videoCodec;

    CHK_STATUS(initKvsWebRtc());
    DLOGI("[KVS Viewer] KVS WebRTC initialization completed successfully");

#ifdef ENABLE_DATA_CHANNEL
    pSampleConfiguration->onDataChannel = onDataChannel;
#endif

    SPRINTF(clientId, "%s_%u", SAMPLE_VIEWER_CLIENT_ID, RAND() % MAX_UINT32);
    CHK_STATUS(initSignaling(pSampleConfiguration, clientId));
    DLOGI("[KVS Viewer] Signaling client connection established");

    // Initialize streaming session
    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;
    CHK_STATUS(createSampleStreamingSession(pSampleConfiguration, NULL, FALSE, &pSampleStreamingSession));
    DLOGI("[KVS Viewer] Creating streaming session...completed");
    pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount++] = pSampleStreamingSession;

    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = FALSE;

    MEMSET(&offerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));
    offerSessionDescriptionInit.useTrickleIce = pSampleStreamingSession->remoteCanTrickleIce;
    CHK_STATUS(setLocalDescription(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));
    DLOGI("[KVS Viewer] Completed setting local description");

    // 【既存】PCM バッファの初期化
    initPCMBuffer(&g_pcmBuffer);

    // ===== [ADDED] ジッターバッファの初期化とスレッド起動 =====
    initAdaptiveJitterBuffer(&g_jitterBuffer, 
                             100, // initialDelayMs
                             20,  // minDelayMs
                             800  // maxDelayMs
    );
    if (pthread_create(&g_jitterThread, NULL, jitterBufferThreadFn, NULL) != 0) {
        DLOGE("Failed to create jitterBuffer thread");
        return EXIT_FAILURE;
    }
    // ===== [END ADDED] =====

    // 再生スレッド起動
    pthread_t playbackTid;
    if (pthread_create(&playbackTid, NULL, playbackThread, NULL) != 0) {
        DLOGE("Failed to create playback thread");
        return EXIT_FAILURE;
    }

    // 音声フレーム受信時のコールバック登録
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
                                  (UINT64) pSampleStreamingSession, sampleAudioFrameHandler3));
    // 映像は従来のハンドラで
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver,
                                  (UINT64) pSampleStreamingSession, sampleVideoFrameHandler));

    if (!pSampleConfiguration->trickleIce) {
        DLOGI("[KVS Viewer] Non trickle ice. Wait for Candidate collection to complete");
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = TRUE;

        while (!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->candidateGatheringDone)) {
            CHK_WARN(!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag), STATUS_OPERATION_TIMED_OUT,
                     "application terminated and candidate gathering still not done");
            CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock,
                      5 * HUNDREDS_OF_NANOS_IN_A_SECOND);
        }

        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = FALSE;

        DLOGI("[KVS Viewer] Candidate collection completed");
    }

    CHK_STATUS(createOffer(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));
    DLOGI("[KVS Viewer] Offer creation successful");

    DLOGI("[KVS Viewer] Generating JSON of session description....");
    CHK_STATUS(serializeSessionDescriptionInit(&offerSessionDescriptionInit, NULL, &buffLen));

    if (buffLen >= SIZEOF(message.payload)) {
        DLOGE("[KVS Viewer] serializeSessionDescriptionInit(): operation returned status code: 0x%08x ",
              STATUS_INVALID_OPERATION);
        retStatus = STATUS_INVALID_OPERATION;
        goto CleanUp;
    }

    CHK_STATUS(serializeSessionDescriptionInit(&offerSessionDescriptionInit, message.payload, &buffLen));

    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_OFFER;
    STRCPY(message.peerClientId, SAMPLE_MASTER_CLIENT_ID);
    message.payloadLen = (buffLen / SIZEOF(CHAR)) - 1;
    message.correlationId[0] = '\0';

    CHK_STATUS(signalingClientSendMessageSync(pSampleConfiguration->signalingClientHandle, &message));

#ifdef ENABLE_DATA_CHANNEL
    PRtcDataChannel pDataChannel = NULL;
    PRtcPeerConnection pPeerConnection = pSampleStreamingSession->pPeerConnection;
    SIZE_T datachannelLocalOpenCount = 0;

    // Creating a new datachannel
    CHK_STATUS(createDataChannel(pPeerConnection, pChannelName, NULL, &pDataChannel));
    DLOGI("[KVS Viewer] Creating data channel...completed");

    CHK_STATUS(dataChannelOnOpen(pDataChannel, (UINT64) &datachannelLocalOpenCount, dataChannelOnOpenCallback));
    DLOGI("[KVS Viewer] Data Channel open now...");
#endif // ENABLE_DATA_CHANNEL

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->interrupted) &&
           !ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag)) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[KVS Viewer] Terminated with status code 0x%08x", retStatus);
    }

    DLOGI("[KVS Viewer] Cleaning up....");

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (pSampleConfiguration->enableFileLogging) {
        freeFileLogger();
    }
    if (pSampleConfiguration != NULL) {
        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS Viewer] freeSignalingClient(): operation returned status code: 0x%08x ", retStatus);
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS Viewer] freeSampleConfiguration(): operation returned status code: 0x%08x ", retStatus);
        }
    }
    DLOGI("[KVS Viewer] Cleanup done");

    // スレッド終了シグナル
    g_running = 0;
    pthread_join(playbackTid, NULL);
    pthread_join(g_jitterThread, NULL);

    RESET_INSTRUMENTED_ALLOCATORS();

    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}