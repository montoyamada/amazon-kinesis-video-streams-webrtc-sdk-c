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
 * 	•	SAMPLE_RATE (48000)
　　　　　　1秒間に48,000サンプルを取得する設定です。
	•	CHANNELS (2)
　　　　　　ステレオ音声であるため、2チャネルを使用します。
	•	FRAME_SIZE (960)
　　　　　　20ms分のサンプル数を指します。たとえば48kHzの場合、1秒は48,000サンプルなので、1msあたり48サンプルとなり、20msでは 48 × 20 = 960 サンプルとなります。
	•	BYTES_PER_SAMPLE (2)
　　　　　　1サンプルは16ビット（= 2バイト）です。
	•	PCM_FRAME_BYTES
　　　　　　1フレームの総バイト数を計算しています。
　　　　　　具体的には、サンプル数（FRAME_SIZE）× チャネル数（CHANNELS）× サンプルあたりバイト数（BYTES_PER_SAMPLE） という式で求めています。
 */
//#define SAMPLE_RATE   48000
//#define CHANNELS      2
//#define FRAME_SIZE    960  // 20ms worth of samples at 48/6 kHz
//#define BYTES_PER_SAMPLE 2        // 16bit = 2 bytes
//#define PCM_FRAME_BYTES (FRAME_SIZE * CHANNELS * BYTES_PER_SAMPLE)
/*
 * 8kHzステレオ, 20msフレーム 
 なぜ FRAME_SIZE が160になるのか
	•	8kHzの場合、1秒間に8000サンプル。
	•	1msあたり 8000 \div 1000 = 8 サンプル。
	•	20ms分のサンプル数は 8 \times 20 = 160。
	•	よって1フレーム（20ms）あたりのサンプル数（片チャネル）は160になる。
フレームあたりの総バイト数
	•	ステレオ(2チャネル)なので 160サンプル/チャネル × 2チャネル = 320サンプル/フレーム。
	•	1サンプルあたり2バイトなので、1フレームあたりの合計バイト数は 320 × 2 = 640 バイト。
 */
#define SAMPLE_RATE       8000               // サンプリング周波数を8kHzに変更
#define CHANNELS          2                  // ステレオ(2チャネル)はそのまま
#define FRAME_SIZE        160                // 20ms分のサンプル数を計算しなおす
#define BYTES_PER_SAMPLE  2                  // 16ビット(2バイト)はそのまま
#define PCM_FRAME_BYTES   (FRAME_SIZE * CHANNELS * BYTES_PER_SAMPLE)

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

        //currentIndexをcurrentIndex.txtに書き込む
        FILE* tmpFp = fopen("currentIndex.tmp", "w");
        if (!tmpFp) {
            fprintf(stderr, "ファイルオープン失敗: %s\n", "currentIndex.tmp");
            continue;
        }
        // 書き込み
        fprintf(tmpFp, "%d\n", currentIndex);
        fclose(tmpFp);
        // 正常終了後にリネーム
        rename("currentIndex.tmp", "currentIndex.txt");

        //------
        //FILE* fp2 = fopen("currentIndex.txt", "w");
        //if (!fp2) {
        //    fprintf(stderr, "ファイルオープン失敗: %s\n", "currentIndex.txt");
        //    continue;
        //}   
        //fprintf(fp2, "%d\n", currentIndex);
        //fclose(fp2);

        // -- NNN を更新 (サイクリックバッファ) --
        currentIndex = (currentIndex + 1) % ringSize;
    }

cleanup:
    opus_encoder_destroy(encoder);
    return 0;
}