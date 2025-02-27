/*****************************************************************************
 * read_from_ring_buffer.c (標準出力対応版) [改造版]
 *
 * 使い方例:
 *   1) 出力をファイルに書き込む場合
 *      ./read_from_ring_buffer 610 20000 decoded_output.pcm
 *
 *   2) 出力を標準出力に書き込む場合
 *      ./read_from_ring_buffer 610 20000 -
 *      (例: パイプで ffmpeg に送る)
 *      ./read_from_ring_buffer 610 20000 - | ffmpeg -f s16le -ar 8000 -ac 2 -i pipe:0 ...
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus/opus.h>
#include <unistd.h>

#define SAMPLE_RATE 8000      // 8kHz
#define CHANNELS    2         // ステレオ
#define FRAME_SIZE  160       // 20msあたり 160サンプル/チャネル
#define MAX_PACKET_SIZE 4000  // エンコード出力の最大バイト数(適当に大きめ)
#define NUMBER_OF_OPUS_FRAME_FILES 618 //Same value as in Samples.h

// 1フレームの出力PCMサンプル数(左右合わせた総サンプル数)は FRAME_SIZE x CHANNELS = 320サンプル
// 16bitなので 320 x 2 = 640 bytes

// currentIndex.txt から現在のインデックスを取得する関数
static int get_current_index(void)
{
    int fileIndex = 0;
    FILE* fp = fopen("currentIndex.txt", "r");
    if (!fp) {
        fprintf(stderr, "[Warning] Failed to open file: currentIndex.txt, defaulting to 0\n");
        return 0;
    }
    fscanf(fp, "%d", &fileIndex);
    fclose(fp);
    return fileIndex;
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <ringSize> <i_usleep> <output.pcm | - (for stdout)>\n", argv[0]);
        fprintf(stderr, "  ringSize: サイクリックバッファの最大数 (例: 610)\n");
        fprintf(stderr, "  i_usleep: usleep に指定する待ち時間 (マイクロ秒)\n");
        fprintf(stderr, "  output.pcm or '-' for stdout\n");
        return 1;
    }

    // リングバッファサイズ
    int ringSize = atoi(argv[1]);
    if (ringSize <= 0) {
        fprintf(stderr, "Invalid ringSize: %d\n", ringSize);
        return 1;
    }

    // usleep 待ち時間 (μs)
    int i_usleep = atoi(argv[2]);
    if (i_usleep <= 0) {
        fprintf(stderr, "Invalid i_usleep: %d\n", i_usleep);
        return 1;
    }

    // 出力先 (ファイル名 or "-" でstdout)
    const char* outPcmFile = argv[3];

    // ------------------------------
    // 1) Opusデコーダの初期化
    int err;
    OpusDecoder* decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if (err != OPUS_OK || decoder == NULL) {
        fprintf(stderr, "Failed to create Opus decoder: %s\n", opus_strerror(err));
        return 1;
    }

    // 出力ストリームを決定
    FILE* fout = NULL;
    int useStdout = 0;

    if (strcmp(outPcmFile, "-") == 0) {
        // 標準出力に書く
        fout = stdout;
        useStdout = 1;
        // バイナリモード (Windows環境など) - Unix系なら不要
        // #ifdef _WIN32
        //   _setmode(_fileno(stdout), _O_BINARY);
        // #endif
    } else {
        // ファイルに書き込み
        fout = fopen(outPcmFile, "wb");
        if (!fout) {
            fprintf(stderr, "Failed to open output file: %s\n", outPcmFile);
            opus_decoder_destroy(decoder);
            return 1;
        }
    }

    // ------------------------------
    int fileIndex_here = get_current_index();
    int fileIndex_here_last = fileIndex_here;   
    int fileIndex = (fileIndex_here >= 0) ? fileIndex_here : 0;
    int i_fileIndex_here_continuous_cnt = 0;

    fprintf(stderr, "decode_from_ring_buffer: ringSize=%d, i_usleep=%d, output=%s\n",
            ringSize, i_usleep, useStdout ? "stdout" : outPcmFile);

    while (1) {
        // 指定されたマイクロ秒だけ待機
        usleep(i_usleep);

        // fileIndex_hereが1000回連続して同じ値だったらループを抜ける
        fileIndex_here = get_current_index();
        //fprintf(stderr, "[Debug] fileIndex_here = %d, i_fileIndex_here_continuous_cnt = %d\n",
        //        fileIndex_here, i_fileIndex_here_continuous_cnt);

        if (fileIndex_here == fileIndex_here_last) {
            i_fileIndex_here_continuous_cnt++;
        } else {
            i_fileIndex_here_continuous_cnt = 0;
        }
        if (i_fileIndex_here_continuous_cnt >= 1000) {
            fprintf(stderr, "[Info] fileIndex_here is continuous for 10 times, break loop.\n");
            break;
        }

        if (fileIndex_here >= 0 && fileIndex_here != fileIndex_here_last) {
            fileIndex = fileIndex_here;
            fileIndex_here_last = fileIndex_here;
        }

        // 入力ファイル (Opusフレーム)
        char inPath[256];
        snprintf(inPath, sizeof(inPath), "opusSampleFrames/sample-%03d.opus", fileIndex);

        FILE* fin = fopen(inPath, "rb");
        if (!fin) {
            fprintf(stderr, "Skipping %s (cannot open)\n", inPath);
            continue;
        }

        fseek(fin, 0, SEEK_END);
        long fileSize = ftell(fin);
        fseek(fin, 0, SEEK_SET);

        if (fileSize <= 0 || fileSize > MAX_PACKET_SIZE) {
            fprintf(stderr, "Skipping %s (invalid size: %ld)\n", inPath, fileSize);
            fclose(fin);
            continue;
        }

        unsigned char encodedData[MAX_PACKET_SIZE];
        size_t readBytes = fread(encodedData, 1, fileSize, fin);
        fclose(fin);

        if (readBytes != (size_t)fileSize) {
            fprintf(stderr, "Read mismatch in %s\n", inPath);
            continue;
        }

        // ------------------------------
        // Opus フレームをデコード => PCM化
        short pcmOut[FRAME_SIZE * CHANNELS];  // 1フレーム用
        int frameCount = opus_decode(decoder,
                                     encodedData,
                                     (opus_int32)fileSize,
                                     pcmOut,
                                     FRAME_SIZE,
                                     0); // no FEC
        if (frameCount < 0) {
            fprintf(stderr, "opus_decode error in %s: %s\n",
                    inPath, opus_strerror(frameCount));
            continue;
        }

        size_t pcmBytes = (size_t)frameCount * CHANNELS * sizeof(short);

        // ------------------------------
        // PCM を出力 (ファイル or stdout)
        if (fwrite(pcmOut, 1, pcmBytes, fout) < pcmBytes) {
            fprintf(stderr, "[Warning] fwrite() incomplete.\n");
        }

        fprintf(stderr, "Decoded: %s -> %zu bytes of PCM\n", inPath, pcmBytes);
    }

    // 終了処理
    if (!useStdout && fout) {
        fclose(fout);
    }
    opus_decoder_destroy(decoder);
    fprintf(stderr, "All frames decoded into %s\n", useStdout ? "stdout" : outPcmFile);

    return 0;
}