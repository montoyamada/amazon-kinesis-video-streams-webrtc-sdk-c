/******************************************************************************
 *  File: kvsWebRTCClientAudioViewerAudioOnly.c
 *
 *  説明:
 *    KVS WebRTC Viewer として起動し、受信音声(Opus)をデコードして
 *    stdout 経由で出力する最小限のサンプルコード。
 *    シェル上でパイプを使って aplay に渡せばリアルタイム再生できる。
 *    例: 
 *      ./kvsWebRTCClientAudioViewerAudioOnly MyChannel opus | \
 *          aplay -f S16_LE -r 16000 -c 1 -t raw
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

// ---- 各種定数定義: 音声のみ ----
#define SAMPLE_RATE         16000
#define CHANNELS            1
#define BYTES_PER_SAMPLE    2
#define FRAME_DURATION_MS   20
#define SAMPLES_PER_FRAME   ((SAMPLE_RATE * FRAME_DURATION_MS) / 1000)  // 320 samples
#define FRAME_SIZE          (SAMPLES_PER_FRAME * BYTES_PER_SAMPLE)      // 640 bytes

// 「簡易的に 5秒分のバッファを確保」とする例
#define PCM_BUFFER_SIZE     (SAMPLE_RATE * BYTES_PER_SAMPLE * 5)        // 16000*2*5 = 160000 bytes

// Opus の最大フレームサンプル数（48000Hz, 120ms 等を想定）
// 実際には 5760 サンプルが上限 (Frame size = 120ms at 48kHz = 5760)
#define MAX_FRAME_SAMPLES   5760

// =============== グローバル/構造体宣言 ===============

// PCMリングバッファ構造体
typedef struct {
    uint8_t data[PCM_BUFFER_SIZE];
    size_t write_index;
    size_t read_index;
    size_t fill_level;  // 現在格納されているバイト数
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PCMBuffer;

// グローバル変数: PCMバッファ & 動作フラグ
static PCMBuffer g_pcmBuffer;
static volatile int g_running = 1;

extern PSampleConfiguration gSampleConfiguration;  // KVS WebRTC サンプルのグローバル設定

// ---- PCMバッファ関連 関数群 ----
static void initPCMBuffer(PCMBuffer* buf)
{
    memset(buf->data, 0, PCM_BUFFER_SIZE);
    buf->write_index = 0;
    buf->read_index = 0;
    buf->fill_level = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->cond, NULL);
}

// PCMデータをバッファにプッシュする（余裕が無ければ古い FRAME_SIZE 分を破棄）
static void pushPCMData(PCMBuffer* buf, const uint8_t* data, size_t len)
{
    pthread_mutex_lock(&buf->mutex);

    // len ぶん追加したときにオーバーするなら、古い1フレームぶん(=FRAME_SIZE)を削除
    while (buf->fill_level + len > PCM_BUFFER_SIZE) {
        buf->read_index = (buf->read_index + FRAME_SIZE) % PCM_BUFFER_SIZE;
        if (buf->fill_level >= FRAME_SIZE) {
            buf->fill_level -= FRAME_SIZE;
        } else {
            buf->fill_level = 0;
        }
    }

    // 書き込み位置から末尾までの余り
    size_t space_to_end = PCM_BUFFER_SIZE - buf->write_index;
    if (len <= space_to_end) {
        memcpy(&buf->data[buf->write_index], data, len);
        buf->write_index = (buf->write_index + len) % PCM_BUFFER_SIZE;
    } else {
        memcpy(&buf->data[buf->write_index], data, space_to_end);
        memcpy(&buf->data[0], data + space_to_end, len - space_to_end);
        buf->write_index = (len - space_to_end);
    }

    buf->fill_level += len;
    pthread_cond_signal(&buf->cond);
    pthread_mutex_unlock(&buf->mutex);
}

// バッファから len バイト分のPCMを読み出す (不足分は読み出せない)
static size_t popPCMData(PCMBuffer* buf, uint8_t* out, size_t len)
{
    pthread_mutex_lock(&buf->mutex);
    size_t available = buf->fill_level;
    size_t to_read = (available >= len) ? len : available;

    if (to_read > 0) {
        size_t space_to_end = PCM_BUFFER_SIZE - buf->read_index;
        if (to_read <= space_to_end) {
            memcpy(out, &buf->data[buf->read_index], to_read);
            buf->read_index = (buf->read_index + to_read) % PCM_BUFFER_SIZE;
        } else {
            memcpy(out, &buf->data[buf->read_index], space_to_end);
            memcpy(out + space_to_end, &buf->data[0], to_read - space_to_end);
            buf->read_index = (to_read - space_to_end);
        }
        buf->fill_level -= to_read;
    }

    pthread_mutex_unlock(&buf->mutex);
    return to_read;
}

// ---- Opus デコード関連 ----
static int decodeOpusFrame(const uint8_t* opusData, size_t opusSize,
                           int16_t* pcmOut, size_t* pcmOutBytes)
{
    // 静的変数: 初回呼び出し時に Opus デコーダを生成し、使いまわす
    static OpusDecoder* decoder = NULL;
    static int decoder_channels = 0;
    int error;

    // === 初回だけ Decoder を作成 ===
    if (decoder == NULL) {
        decoder = opus_decoder_create(48000, 2, &error);  // 仮に48kHz,ステレオで生成
        if (error != OPUS_OK) {
            fprintf(stderr, "Failed to create Opus decoder: %s\n", opus_strerror(error));
            return -1;
        }
        decoder_channels = 2;
    }

    // ---- Opus デコード先の一時バッファ(ステレオ max) ----
    int16_t decoded[MAX_FRAME_SAMPLES * 2];
    int samples_per_channel = opus_decode(decoder,
                                          opusData,
                                          opusSize,
                                          decoded,
                                          MAX_FRAME_SAMPLES,
                                          0 /* FEC */);
    if (samples_per_channel < 0) {
        fprintf(stderr, "Opus decoding error: %s\n", opus_strerror(samples_per_channel));
        return -1;
    }

    // ---- ステレオ→モノラルにダウンミックス (2chなら平均を取る) ----
    //     decoder_channelsが1ならそのままコピー
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

    // ---- 48kHz→16kHz へダウンサンプリング（単純に1/3 抽出）----
    int targetSamples = samples_per_channel / 3;
    if (targetSamples > SAMPLES_PER_FRAME) {
        targetSamples = SAMPLES_PER_FRAME;  // 過剰分は切る
    }

    for (int i = 0; i < targetSamples; i++) {
        pcmOut[i] = mono[i * 3];
    }

    *pcmOutBytes = targetSamples * sizeof(int16_t);
    return 0;
}

// 音声フレーム受信コールバック
// (KVS SDKの transceiverOnFrame(...) から呼ばれる)
VOID sampleAudioFrameHandler3(UINT64 customData, PFrame pFrame)
{
    (void)customData;  // 未使用

    int16_t pcmData[SAMPLES_PER_FRAME];
    size_t pcmBytes = 0;
    if (decodeOpusFrame(pFrame->frameData, pFrame->size, pcmData, &pcmBytes) == 0) {
        // デコード成功したPCMをリングバッファに格納
        pushPCMData(&g_pcmBuffer, (const uint8_t*) pcmData, pcmBytes);
    }
}

// ---- 再生用スレッド: バッファ→stdout に書き出し ----
static void* playbackThread(void* arg)
{
    (void) arg;
    FILE* outFile = stdout;

    uint8_t frameBuffer[FRAME_SIZE];
    uint8_t silence[FRAME_SIZE];
    memset(silence, 0, FRAME_SIZE);

    while (g_running) {
        // 1フレーム(=FRAME_SIZE)ぶんポップ
        size_t gotBytes = popPCMData(&g_pcmBuffer, frameBuffer, FRAME_SIZE);
        if (gotBytes < FRAME_SIZE) {
            // 足りない分だけ無音を補う
            if (gotBytes > 0) {
                fwrite(frameBuffer, 1, gotBytes, outFile);
            }
            fwrite(silence, 1, FRAME_SIZE - gotBytes, outFile);
        } else {
            fwrite(frameBuffer, 1, FRAME_SIZE, outFile);
        }
        fflush(outFile);

        // 20msフレームごとにスリープ
        usleep(FRAME_DURATION_MS * 1000);
    }
    return NULL;
}

// ---- シグナルハンドラ (Ctrl+C) ----
#ifndef _WIN32
static void sigintHandler_here(int signum)
{
    (void)signum;
    ATOMIC_STORE_BOOL(&gSampleConfiguration->interrupted, TRUE);
    g_running = 0;  // 再生スレッド停止用
}
#endif

// メイン関数（音声のみの超簡易版）
INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingMessage offerMsg;
    RtcSessionDescriptionInit offerDesc;
    UINT32 buffLen = 0;
    PSampleConfiguration pSampleConfig = NULL;
    PSampleStreamingSession pStreamingSession = NULL;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    PCHAR pChannelName;
    CHAR clientId[256];

    // チャンネル名とCodecを引数から取得
    pChannelName = (argc > 1) ? argv[1] : (PCHAR) "TestChannel";
    if (argc > 2 && !STRCMP(argv[2], AUDIO_CODEC_NAME_OPUS)) {
        audioCodec = RTC_CODEC_OPUS; // (本サンプルはOPUS想定)
    }

#ifndef _WIN32
    signal(SIGINT, sigintHandler_here);
#endif

    // 1) KVS用サンプル設定を用意
    UINT32 logLevel = setLogLevel();  // 簡易: ログレベルをENV等から
    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_VIEWER,
                                         TRUE, TRUE, logLevel, &pSampleConfig));
    pSampleConfig->mediaType = SAMPLE_STREAMING_AUDIO_ONLY;
    pSampleConfig->audioCodec = audioCodec;

    // 2) KVS WebRTC SDK の初期化
    CHK_STATUS(initKvsWebRtc());
    printf("[KVS Viewer] initKvsWebRtc done.\n");

    // 3) シグナリングクライアント接続
    SPRINTF(clientId, "%s_%u", SAMPLE_VIEWER_CLIENT_ID, RAND() % MAX_UINT32);
    CHK_STATUS(initSignaling(pSampleConfig, clientId));
    printf("[KVS Viewer] Signaling client connected.\n");

    // 4) Streaming Session の生成
    CHK_STATUS(createSampleStreamingSession(pSampleConfig, NULL, FALSE, &pStreamingSession));
    printf("[KVS Viewer] createSampleStreamingSession done.\n");
    pSampleConfig->sampleStreamingSessionList[pSampleConfig->streamingSessionCount++] = pStreamingSession;

    // 5) ローカル記述を設定
    MEMSET(&offerDesc, 0, SIZEOF(RtcSessionDescriptionInit));
    offerDesc.useTrickleIce = pStreamingSession->remoteCanTrickleIce;
    CHK_STATUS(setLocalDescription(pStreamingSession->pPeerConnection, &offerDesc));
    printf("[KVS Viewer] setLocalDescription done.\n");

    // 6) フレーム受信コールバック登録 (音声のみ)
    CHK_STATUS(transceiverOnFrame(pStreamingSession->pAudioRtcRtpTransceiver,
                                  (UINT64) pStreamingSession,
                                  sampleAudioFrameHandler3));

    // 7) ノントリクルアイスならアイス候補Gathering完了まで待つ
    if (!pSampleConfig->trickleIce) {
        while (!ATOMIC_LOAD_BOOL(&pStreamingSession->candidateGatheringDone) &&
               !ATOMIC_LOAD_BOOL(&pStreamingSession->terminateFlag)) {
            THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
        }
        printf("[KVS Viewer] ICE candidate gathering done.\n");
    }

    // 8) createOffer & signaling 送信
    CHK_STATUS(createOffer(pStreamingSession->pPeerConnection, &offerDesc));
    printf("[KVS Viewer] createOffer done.\n");

    // JSON 化
    CHK_STATUS(serializeSessionDescriptionInit(&offerDesc, NULL, &buffLen));
    if (buffLen >= SIZEOF(offerMsg.payload)) {
        printf("[KVS Viewer] Offer serialization too large.\n");
        retStatus = STATUS_INVALID_OPERATION;
        goto CleanUp;
    }
    CHK_STATUS(serializeSessionDescriptionInit(&offerDesc, offerMsg.payload, &buffLen));

    // オファーメッセージ送信
    offerMsg.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    offerMsg.messageType = SIGNALING_MESSAGE_TYPE_OFFER;
    STRCPY(offerMsg.peerClientId, SAMPLE_MASTER_CLIENT_ID);
    offerMsg.payloadLen = (buffLen / SIZEOF(CHAR)) - 1;
    offerMsg.correlationId[0] = '\0';
    CHK_STATUS(signalingClientSendMessageSync(pSampleConfig->signalingClientHandle, &offerMsg));
    printf("[KVS Viewer] Offer sent.\n");

    // === PCMバッファ初期化 & 再生用スレッド起動 ===
    initPCMBuffer(&g_pcmBuffer);
    pthread_t playbackTid;
    if (pthread_create(&playbackTid, NULL, playbackThread, NULL) != 0) {
        printf("[KVS Viewer] Failed to create playback thread.\n");
        retStatus = STATUS_INVALID_OPERATION;
        goto CleanUp;
    }
    printf("[KVS Viewer] Playback thread started.\n");

    // === イベントループ: Ctrl+C などで終了するまで待機 ===
    while (!ATOMIC_LOAD_BOOL(&pSampleConfig->interrupted) &&
           !ATOMIC_LOAD_BOOL(&pStreamingSession->terminateFlag)) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

CleanUp:
    printf("[KVS Viewer] Cleaning up...\n");
    g_running = 0;
    pthread_join(playbackTid, NULL);

    if (pSampleConfig != NULL) {
        freeSignalingClient(&pSampleConfig->signalingClientHandle);
        freeSampleConfiguration(&pSampleConfig);
    }
    printf("[KVS Viewer] Cleanup done.\n");

    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}