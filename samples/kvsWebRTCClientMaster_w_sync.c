/****************************************************************************
 * kvsWebRTCClientMaster.c
 *
 * 参考: amazon-kinesis-video-streams-webrtc-sdk-c/samples
 *       の kvsWebRTCClientMaster.c をベースにし、
 *       音声スレッドが異常終了しても再度起動するための改造を加えたサンプルコード。
 *
 ****************************************************************************/
#include "Samples.h"

extern PSampleConfiguration gSampleConfiguration;

//
// 無音やエラーで「音声スレッドの再起動が必要」と判断したときに TRUE をセットする
// SampleConfiguration のメンバを利用する:
//   volatile SIZE_T needToRestartAudioThread;
//
///////////////////////////////////////////////////////////////////////////

/*****************************************************************************
 * 追加：無音カウンタの閾値など
 *****************************************************************************/
#define SILENT_FRAME_THRESHOLD 100

/*****************************************************************************
 * Utility:  ファイル読み込み
 *  - 既存 Samples.c にある readFrameFromDisk とほぼ同様ですが、
 *    「pFrame == NULL の場合はサイズ取得のみ」というロジックを追加することが多いです。
 *  - ここではサンプルとして、ファイルが読めなければ *pSize=0 にしてエラー返す簡易版。
 *****************************************************************************/
STATUS readFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;
    FILE* fp = NULL;
    CHK_ERR(pSize != NULL && frameFilePath != NULL, STATUS_NULL_ARG, "readFrameFromDisk: invalid argument");

    // まずサイズ取得用にファイルを開く
    fp = fopen(frameFilePath, "rb");
    if (fp == NULL) {
        DLOGW("Failed to open file: %s", frameFilePath);
        retStatus = STATUS_INTERNAL_ERROR;
        CHK(FALSE, "file open error");
    }

    CHK_ERR(fseek(fp, 0L, SEEK_END) == 0, STATUS_INTERNAL_ERROR, "fseek SEEK_END failed");
    size = (UINT64) ftell(fp);
    CHK_ERR(fseek(fp, 0L, SEEK_SET) == 0, STATUS_INTERNAL_ERROR, "fseek SEEK_SET failed");

    if (pFrame == NULL) {
        // サイズ取得のみ
        *pSize = (UINT32) size;
        CHK(FALSE, "size only");
    } else {
        // 実データ読み込み
        CHK_ERR(fread(pFrame, 1, (size_t) size, fp) == size, STATUS_INTERNAL_ERROR, "fread frame data error");
        *pSize = (UINT32) size;
    }

CleanUp:
    if (fp != NULL) {
        fclose(fp);
    }
    return retStatus;
}

/*****************************************************************************
 * sendVideoPackets: 既存のサンプルと同様、H.264/H.265ファイルをループ再生
 *****************************************************************************/
PVOID sendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    RtcEncoderStats encoderStats;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT64 startTime, lastFrameTime, elapsed;
    UINT32 i;

    MEMSET(&encoderStats, 0x00, SIZEOF(RtcEncoderStats));
    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");

    frame.presentationTs = 0;
    startTime = GETTIME();
    lastFrameTime = startTime;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_H264_FRAME_FILES + 1;
        if (pSampleConfiguration->videoCodec == RTC_CODEC_H265) {
            SNPRINTF(filePath, MAX_PATH_LEN, "./h265SampleFrames/frame-%04d.h265", fileIndex);
        } else {
            // デフォルトをH.264とする
            SNPRINTF(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%04d.h264", fileIndex);
        }

        // フレームサイズ取得
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            pSampleConfiguration->pVideoFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pVideoFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pVideoFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate video frame buffer");
            pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = frameSize;

        // 実フレーム読み込み
        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));
        frame.size = frameSize;

        // ビデオ時間を積算
        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;

        // 書き込み
        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);

            if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
            }

            // SRTPがまだ準備できていないときのリトライ制御
            if (status == STATUS_SRTP_NOT_READY_YET) {
                // 次ループでKeyフレームからスタートするためにfileIndexをリセット
                fileIndex = 0;
            } else if (status != STATUS_SUCCESS) {
                DLOGV("writeFrame(video) failed with 0x%08x", status);
            }

            // stats更新のサンプル(固定値)
            encoderStats.width = 640;
            encoderStats.height = 480;
            encoderStats.targetBitrate = 262000;
            encoderStats.encodeTimeMsec = 4;
            updateEncoderStats(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &encoderStats);
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);

        // フレーム間インターバル分だけスリープ
        elapsed = lastFrameTime - startTime;
        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION - elapsed % SAMPLE_VIDEO_FRAME_DURATION);
        lastFrameTime = GETTIME();
    }

CleanUp:
    DLOGI("[KVS Master] Closing video thread");
    CHK_LOG_ERR(retStatus);

    return (PVOID) (ULONG_PTR) retStatus;
}

/*****************************************************************************
 * sendAudioPackets: 変更点：無音(=ファイルサイズ0バイト)が続いたら再起動フラグを立てる
 *****************************************************************************/
PVOID sendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT32 i;
    UINT32 silentCount = 0;  // 連続で“ファイルサイズ0”が続いた回数

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    frame.presentationTs = 0;

    // スレッド開始時に「再起動要求フラグ」をオフ
    ATOMIC_STORE(&pSampleConfiguration->needToRestartAudioThread, (SIZE_T)FALSE);

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        // ファイル番号をローテーション
        fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES + 1;

        // OPUSファイルパス生成
        SNPRINTF(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);

        // ファイルサイズだけ先に取得
        frameSize = 0;
        retStatus = readFrameFromDisk(NULL, &frameSize, filePath);
        if (retStatus != STATUS_SUCCESS || frameSize == 0) {
            // 読み込み失敗 or サイズ0 = 無音フレームとみなす
            silentCount++;
            if (silentCount >= SILENT_FRAME_THRESHOLD) {
                DLOGW("[KVS Master] Detected too many silent frames. Requesting audio thread restart...");
                // 「再起動フラグ」ON
                ATOMIC_STORE(&pSampleConfiguration->needToRestartAudioThread, (SIZE_T)TRUE);
                break; // スレッド終了
            }

            // 次のフレームへ
            THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
            continue;
        }

        // 無音でなかったのでカウンタリセット
        silentCount = 0;

        // バッファ拡張チェック
        if (frameSize > pSampleConfiguration->audioBufferSize) {
            pSampleConfiguration->pAudioFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pAudioFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pAudioFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate audio frame buffer");
            pSampleConfiguration->audioBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
        frame.size = frameSize;
        // 実データ読み込み
        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));
        frame.size = frameSize;

        // タイムスタンプを積算
        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        // 各セッションに対して書き込み
        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);

            // SRTP未準備ならリセット
            if (status == STATUS_SRTP_NOT_READY_YET) {
                fileIndex = 0;
            } else if (status != STATUS_SUCCESS) {
                DLOGV("writeFrame(audio) failed with 0x%08x", status);
            } else if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame) {
                // 初回フレーム時間記録
                PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);

        // フレーム間隔
        THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
    }

CleanUp:
    DLOGI("[KVS Master] closing audio thread");
    return (PVOID) (ULONG_PTR) retStatus;
}

/*****************************************************************************
 * sampleReceiveAudioVideoFrame: 受信フレームハンドラ (サンプルそのまま)
 *****************************************************************************/
PVOID sampleReceiveAudioVideoFrame(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    CHK_ERR(pSampleStreamingSession != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");

    // 受信したフレームが来たときに呼ばれるコールバックをセット
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver,
                                  (UINT64) pSampleStreamingSession,
                                  sampleVideoFrameHandler));
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
                                  (UINT64) pSampleStreamingSession,
                                  sampleAudioFrameHandler));

CleanUp:
    return (PVOID) (ULONG_PTR) retStatus;
}

/*****************************************************************************
 * main:  変更ポイント -> “ループ構造”で音声スレッド再起動対応
 *****************************************************************************/
INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 frameSize;
    PSampleConfiguration pSampleConfiguration = NULL;
    PCHAR pChannelName;
    SignalingClientMetrics signalingClientMetrics;
    signalingClientMetrics.version = SIGNALING_CLIENT_METRICS_CURRENT_VERSION;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();

    DLOGI("[KVS Master] Starting KVS Master (audio-thread-restart version)");

#ifndef _WIN32
    signal(SIGINT, sigintHandler); // Ctrl+C割り込み
#endif

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = argc > 1 ? argv[1] : GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif

    // サンプル設定を作成
    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
                                         TRUE, TRUE, logLevel, &pSampleConfiguration));

    // コマンドライン引数で音声コーデック指定（例）
    if (argc > 3) {
        if (!STRCMP(argv[3], AUDIO_CODEC_NAME_OPUS)) {
            audioCodec = RTC_CODEC_OPUS;
        }
    }

    // コマンドライン引数で映像コーデック指定（例）
    if (argc > 4) {
        if (!STRCMP(argv[4], VIDEO_CODEC_NAME_H265)) {
            videoCodec = RTC_CODEC_H265;
        } else {
            DLOGI("[KVS Master] Defaulting to H264 as the specified codec's sample frames may not be available");
        }
    }

    // 音声・映像・受信ハンドラを割り当て
    pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioVideoFrame;
    pSampleConfiguration->audioCodec = audioCodec;
    pSampleConfiguration->videoCodec = videoCodec;

    // rolling bufferなど(省略) - 必要に応じて
    if (videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
        pSampleConfiguration->videoRollingBufferDurationSec = 3;
        pSampleConfiguration->videoRollingBufferBitratebps = 1.4 * 1024 * 1024;
    } else if (videoCodec == RTC_CODEC_H265) {
        pSampleConfiguration->videoRollingBufferDurationSec = 3;
        pSampleConfiguration->videoRollingBufferBitratebps = 462 * 1024;
    }
    if (audioCodec == RTC_CODEC_OPUS) {
        pSampleConfiguration->audioRollingBufferDurationSec = 3;
        pSampleConfiguration->audioRollingBufferBitratebps = 512 * 1024;
    }

    if (argc > 2 && STRNCMP(argv[2], "1", 2) == 0) {
        pSampleConfiguration->channelInfo.useMediaStorage = TRUE;
    }

#ifdef ENABLE_DATA_CHANNEL
    pSampleConfiguration->onDataChannel = onDataChannel;
#endif
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    DLOGI("[KVS Master] Finished setting handlers");

    // サンプルフレームのチェック（H.264/H.265/Opusファイルが存在するか？）
    if (videoCodec == RTC_CODEC_H265) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h265SampleFrames/frame-0001.h265"));
        DLOGI("[KVS Master] Checked H265 sample video frame availability....available");
    } else {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-0001.h264"));
        DLOGI("[KVS Master] Checked H264 sample video frame availability....available");
    }
    if (audioCodec == RTC_CODEC_OPUS) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus"));
        DLOGI("[KVS Master] Checked Opus sample audio frame availability....available");
    }

    // KVS WebRTC の初期化(一度だけ or 再起動のたびに)
    CHK_STATUS(initKvsWebRtc());
    DLOGI("[KVS Master] KVS WebRTC initialization completed successfully");

    // Signalingクライアントを初期化し、チャンネルに接続する
    PROFILE_CALL_WITH_START_END_T_OBJ(
        retStatus = initSignaling(pSampleConfiguration, SAMPLE_MASTER_CLIENT_ID),
        pSampleConfiguration->signalingClientMetrics.signalingStartTime,
        pSampleConfiguration->signalingClientMetrics.signalingEndTime,
        pSampleConfiguration->signalingClientMetrics.signalingCallTime,
        "Initialize signaling client and connect to the signaling channel");

    DLOGI("[KVS Master] Channel %s set up done ", pChannelName);

    // ここから、何度でも「セッションが落ちても再起動」できるように while で囲む
    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        // ストリーミング終了まで待つ
        // (複数回路接続などがありうるため、セッション終了条件は sessionCleanupWait 次第)
        CHK_STATUS(sessionCleanupWait(pSampleConfiguration));
        DLOGI("[KVS Master] Streaming session terminated");

        // 音声送信スレッドをJOIN (終了していなければ)
        if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
            THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
            pSampleConfiguration->mediaSenderTid = INVALID_TID_VALUE;
        }

        // 再起動フラグ確認
        if (ATOMIC_LOAD(&pSampleConfiguration->needToRestartAudioThread)) {
            DLOGI("[KVS Master] Detected audio thread restart request -> Re-initializing");
            // フラグを下ろす
            ATOMIC_STORE(&pSampleConfiguration->needToRestartAudioThread, (SIZE_T)FALSE);

            // 必要ならファイルIndexやpresentationTs等をリセット
            // (例) pSampleConfiguration->someFileIndex = 0; etc...

            // もしSignalingClientやPeerConnectionを作り直すならここで再初期化
            //   -> ただし sample では createPeerConnectionEach time 方式の場合、
            //      sessionCleanupWait後に再度 create or init するロジックが必要。
            //   -> ここでは簡単のため "while(true)" から抜けずに同じclientを使い回す

            // ループ先頭に戻り、また sessionCleanupWait などを待機 (＝再度音声スレッドが起動)
            // ※ ただし、実際のサンプルでは "新規セッションをどう開始するか" のロジックがあり、
            //    たとえば "viewerからの接続を待つ" などの場合は別途 handleNewPeerConnection が必要。
            //    シンプルに「無限ループで待ち+新規セッションで音声スレッド再起動」という設計なら
            //    追加の再初期化コードを書く。
            continue;
        } else {
            // 再起動不要 -> 正常終了か、またはユーザー終了
            DLOGI("[KVS Master] No audio restart needed. Exiting main loop.");
            ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);
        }
    }

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[KVS Master] Terminated with status code 0x%08x", retStatus);
    }

    DLOGI("[KVS Master] Cleaning up....");
    if (pSampleConfiguration != NULL) {
        // 終了フラグをセット
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
            THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
        }

        // SignalingClientメトリクス
        if (STATUS_SUCCESS == signalingClientGetMetrics(pSampleConfiguration->signalingClientHandle, &signalingClientMetrics)) {
            logSignalingClientStats(&signalingClientMetrics);
        }

        // SignalingClient解放
        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS Master] freeSignalingClient() returned status code: 0x%08x", retStatus);
        }

        // SampleConfiguration解放
        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS Master] freeSampleConfiguration() returned status code: 0x%08x", retStatus);
        }
    }

    DLOGI("[KVS Master] Cleanup done");
    CHK_LOG_ERR(retStatus);

    RESET_INSTRUMENTED_ALLOCATORS();

    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}