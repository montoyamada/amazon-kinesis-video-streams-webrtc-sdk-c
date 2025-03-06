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
#include <opus/opus.h>

// PCM再生用パラメータ定義
#define SAMPLE_RATE         16000
#define CHANNELS            1
#define BYTES_PER_SAMPLE    2
#define FRAME_DURATION_MS   20
#define SAMPLES_PER_FRAME   ((SAMPLE_RATE * FRAME_DURATION_MS) / 1000)  // 320 samples
#define FRAME_SIZE          (SAMPLES_PER_FRAME * BYTES_PER_SAMPLE)      // 640 bytes
#define PCM_BUFFER_SIZE     (SAMPLE_RATE * BYTES_PER_SAMPLE * 5)        // 5秒分（例: 16000*2*5 = 160000 バイト）
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
// onMessage callback for a message received by the viewer on a data channel
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

// onOpen callback for the onOpen event of a viewer created data channel
VOID dataChannelOnOpenCallback(UINT64 customData, PRtcDataChannel pDataChannel)
{
    STATUS retStatus = STATUS_SUCCESS;
    DLOGI("New DataChannel has been opened %s ", pDataChannel->name);
    dataChannelOnMessage(pDataChannel, customData, dataChannelOnMessageCallback);
    ATOMIC_INCREMENT((PSIZE_T) customData);
    // Sending first message to the master over the data channel
    retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) VIEWER_DATA_CHANNEL_MESSAGE, STRLEN(VIEWER_DATA_CHANNEL_MESSAGE));
    if (retStatus != STATUS_SUCCESS) {
        DLOGI("[KVS Viewer] dataChannelSend(): operation returned status code: 0x%08x ", retStatus);
    }
}
#endif // ENABLE_DATA_CHANNEL

// PCMバッファ初期化
void initPCMBuffer(PCMBuffer* buf) {
    memset(buf->data, 0, PCM_BUFFER_SIZE);
    buf->write_index = 0;
    buf->read_index = 0;
    buf->fill_level = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->cond, NULL);
}

// PCMデータをバッファにプッシュする（余裕がない場合は古い FRAME_SIZE 分を破棄）
void pushPCMData(PCMBuffer* buf, uint8_t* data, size_t len) {
    pthread_mutex_lock(&buf->mutex);

    // 新規データを入れるための空きが無ければ、古いフレームを捨てる
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

// バッファからPCMデータをポップする（不足分は読み出せないので caller で無音補完）
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

// Opus フレームを 16kHz/mono PCM にデコードする
int decodeOpusFrame(const uint8_t* opusData, size_t opusSize, int16_t* pcmOut, size_t* pcmOutSize) {
    static OpusDecoder *decoder = NULL;
    static int decoder_channels = 0;
    int error;

    // 初回呼び出し時に OpusDecoder を生成（ここでは 48000Hz, 2チャンネル を仮定）
    if (!decoder) {
        decoder = opus_decoder_create(48000, 2, &error);
        if (error != OPUS_OK) {
            fprintf(stderr, "Failed to create Opus decoder: %s\n", opus_strerror(error));
            return error;
        }
        decoder_channels = 2;
    }

    // デコード結果を格納する一時バッファ
    // (インタリーブされたPCM：チャンネル数分のサンプル×samples_per_channel)
    int16_t decoded[MAX_FRAME_SAMPLES * decoder_channels];
    int samples_per_channel = opus_decode(decoder, opusData, opusSize,
                                          decoded, MAX_FRAME_SAMPLES, 0);
    if (samples_per_channel < 0) {
        fprintf(stderr, "Opus decoding error: %s\n", opus_strerror(samples_per_channel));
        return samples_per_channel;
    }

    // モノラルにダウンミックス（decoder_channels が 1 ならそのまま、2 以上の場合は平均）
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

    // 48000Hz から 16000Hz への変換は、
    // ダウンサンプリング率 3 で実施（単純に 3 分の 1 のサンプルを抽出）
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

// 音声フレーム受信コールバック
VOID sampleAudioFrameHandler3(UINT64 customData, PFrame pFrame)
{
    UNUSED_PARAM(customData);
    DLOGV("Audio Frame received. TrackId: %" PRIu64 ", Size: %u, Flags %u",
          pFrame->trackId, pFrame->size, pFrame->flags);

    int16_t pcmData[SAMPLES_PER_FRAME];
    size_t pcmDataSize = 0;
    if (decodeOpusFrame(pFrame->frameData, pFrame->size, pcmData, &pcmDataSize) != 0) {
        DLOGE("Opus decoding failed for TrackId: %" PRIu64, pFrame->trackId);
        return;
    }
    pushPCMData(&g_pcmBuffer, (uint8_t*)pcmData, pcmDataSize);
}

// 再生スレッド：バッファから PCM データを取り出し、標準出力へ書き出す
void* playbackThread(void* arg) {
    (void)arg;

    // aplay を外部パイプで繋ぐ場合はプログラムは標準出力に書くだけ
    FILE* outFile = stdout;

    uint8_t frameBuffer[FRAME_SIZE];
    uint8_t silentFrame[FRAME_SIZE];
    memset(silentFrame, 0, FRAME_SIZE);

    while (g_running) {
        size_t bytesRead = popPCMData(&g_pcmBuffer, frameBuffer, FRAME_SIZE);
        if (bytesRead < FRAME_SIZE) {
            // 一部だけ読み込めた場合は、その分だけ書いて残りは無音
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

    // outFile が stdout の場合、ここでは閉じない
    // (閉じると他のログ出力が止まる可能性あり)
    return NULL;
}

// SIGINT (Ctrl + C) で呼ばれるハンドラ (元サンプルが用意している想定)
#ifndef _WIN32
static void sigintHandler(int signum)
{
    UNUSED_PARAM(signum);
    ATOMIC_STORE_BOOL(&gSampleConfiguration->interrupted, TRUE);
    g_running = 0;  // 再生スレッド停止用
}
#endif

// メイン関数
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
    signal(SIGINT, sigintHandler);
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

    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
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

    // 【追加処理】PCM バッファの初期化と再生スレッド起動
    initPCMBuffer(&g_pcmBuffer);
    pthread_t playbackTid;
    if (pthread_create(&playbackTid, NULL, playbackThread, NULL) != 0) {
        DLOGE("Failed to create playback thread");
        return EXIT_FAILURE;
    }

    // 音声・映像フレーム受信時のコールバック登録（音声は更新済み sampleAudioFrameHandler を利用）
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
                                  (UINT64) pSampleStreamingSession, sampleAudioFrameHandler3));
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver,
                                  (UINT64) pSampleStreamingSession, sampleVideoFrameHandler));

    if (!pSampleConfiguration->trickleIce) {
        DLOGI("[KVS Viewer] Non trickle ice. Wait for Candidate collection to complete");
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = TRUE;

        while (!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->candidateGatheringDone)) {
            CHK_WARN(!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag), STATUS_OPERATION_TIMED_OUT,
                     "application terminated and candidate gathering still not done");
            CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock, 5 * HUNDREDS_OF_NANOS_IN_A_SECOND);
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

    // Creating a new datachannel on the peer connection of the existing sample streaming session
    CHK_STATUS(createDataChannel(pPeerConnection, pChannelName, NULL, &pDataChannel));
    DLOGI("[KVS Viewer] Creating data channel...completed");

    // Setting a callback for when the data channel is open
    CHK_STATUS(dataChannelOnOpen(pDataChannel, (UINT64) &datachannelLocalOpenCount, dataChannelOnOpenCallback));
    DLOGI("[KVS Viewer] Data Channel open now...");
#endif // ENABLE_DATA_CHANNEL

    // Block until interrupted
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

    // 再生スレッド終了のシグナル送信と join
    g_running = 0;
    pthread_join(playbackTid, NULL);

    RESET_INSTRUMENTED_ALLOCATORS();

    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}