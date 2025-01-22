#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <opus/opus.h>
#include <unistd.h>
/*
 * 48kHzステレオ, 20msフレーム = 960サンプル/チャネル
 * 1サンプル = 16bit(2byte)
 * 2チャンネルなので 960*2=1920 サンプル, バイト数=3840
 */
#define SAMPLE_RATE   48000
#define CHANNELS      2
#define FRAME_SIZE    960         // 20ms worth of samples at 48 kHz
#define BYTES_PER_SAMPLE 2        // 16bit = 2 bytes
#define PCM_FRAME_BYTES (FRAME_SIZE * CHANNELS * BYTES_PER_SAMPLE)

// エンコード出力の最大サイズ(十分な余裕を持たせる)
#define MAX_OPUS_PACKET_SIZE 4000

#define APPLICATION OPUS_APPLICATION_AUDIO  // 汎用音声用途

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <MMM>\n", argv[0]);
        fprintf(stderr, "  MMM: サイクリックバッファの最大数\n");
        return 1;
    }

    // リングバッファサイズ
    int ringSize = atoi(argv[1]);
    if (ringSize <= 0) {
        fprintf(stderr, "Invalid ringSize: %d\n", ringSize);
        return 1;
    }

    // -- Opus Encoder の初期化 --
    int err;
    OpusEncoder* encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, APPLICATION, &err);
    if (err != OPUS_OK) {
        fprintf(stderr, "Opusエンコーダの初期化に失敗しました: %s\n", opus_strerror(err));
        return 1;
    }

    // ビットレート等を設定（必要に応じて調整してください）
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000)); // 64kbps
    // VBR, CBR などの設定も可能 (例: opus_encoder_ctl(encoder, OPUS_SET_VBR(0)); でCBR化)

    unsigned char pcmBuffer[PCM_FRAME_BYTES];
    unsigned char encodedData[MAX_OPUS_PACKET_SIZE];

    //Wait time for usleep
    int i_usleep = atoi(argv[2]);
    printf("i_usleep = %d usec\n", i_usleep);
    int currentIndex = 0;  // NNN
    while (1) {
        //i_usleep(usec)ごとにPCMを読み込み、Opusエンコードしてファイルに書き込む
        //cat *.wav | this_programの場合のパイプライン速度調整
        usleep(i_usleep); // u_sleep usec秒待つ
        // -- 20ms (3840バイト) のPCMを読み込む --
        size_t bytesRead = 0;
        size_t totalRead = 0;

        // fread が分割される可能性があるので、ループで 3840バイト埋まるまで読む（簡易実装）
        while (totalRead < PCM_FRAME_BYTES) {
            bytesRead = fread(pcmBuffer + totalRead, 1, PCM_FRAME_BYTES - totalRead, stdin);
            if (bytesRead == 0) {
                // EOF またはエラー
                goto cleanup;
            }
            totalRead += bytesRead;
        }

        // -- Opusエンコード --
        // 16bit PCM => short型にキャストしてopus_encode()に渡す。
        // pcmBuffer を short配列とみなす
        const short* pcmShort = (const short*) pcmBuffer;

        // フレームをエンコード
        // return 値 = エンコード後のバイト数
        int encodedBytes = opus_encode(encoder, pcmShort, FRAME_SIZE, encodedData, MAX_OPUS_PACKET_SIZE);
        if (encodedBytes < 0) {
            fprintf(stderr, "Opusエンコードに失敗: %s\n", opus_strerror(encodedBytes));
            continue;  // このフレームは飛ばし、次フレームでリトライ
        }

        // -- ファイル名を生成 & 書き込み --
        // audio_(NNN).opus
        char filePath[256];
        //snprintf(filePath, sizeof(filePath), "ring_buffer/sample-%03d.opus", currentIndex);
        snprintf(filePath, sizeof(filePath), "opusSampleFrames/sample-%03d.opus", currentIndex);
        FILE* fp = fopen(filePath, "wb");
        if (!fp) {
            fprintf(stderr, "ファイルオープン失敗: %s\n", filePath);
            continue;
        }
        fwrite(encodedData, 1, encodedBytes, fp);
        fclose(fp);

        // -- 標準出力に現在のインデックスを表示 --
        //   (他のアプリケーションがこの出力を見て、どのファイルに書かれたか把握できる)
        // ファイル渡しにしたためにとりあえずコメントアウト
        //printf("%d\n", currentIndex);
        //fflush(stdout);

        //currentIndexをcurrentIndex.txtに書き込む
        FILE* fp2 = fopen("currentIndex.txt", "w");
        if (!fp2) {
            fprintf(stderr, "ファイルオープン失敗: %s\n", "currentIndex.txt");
            continue;
        }   
        fprintf(fp2, "%d\n", currentIndex);
        fclose(fp2);

        // -- NNN を更新 (サイクリックバッファ) --
        currentIndex = (currentIndex + 1) % ringSize;
    }

cleanup:
    opus_encoder_destroy(encoder);
    return 0;
}