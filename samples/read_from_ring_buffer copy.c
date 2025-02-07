#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <opus/opus.h>
#include <unistd.h>
// パラメータは「save_to_ring_buffer」プログラムとあわせる
#define SAMPLE_RATE 8000      // 8kHz
#define CHANNELS    2         // ステレオ
#define FRAME_SIZE  160       // 20msあたり 160サンプル/チャネル
#define MAX_PACKET_SIZE 4000  // エンコード出力の最大バイト数(適当に大きめ)
#define NUMBER_OF_OPUS_FRAME_FILES               618 //Same value as in Samples.h

// 1フレームの出力PCMサンプル数(左右合わせた総サンプル数)は FRAME_SIZE x CHANNELS = 320サンプル
// 16bitなので 320 x 2 = 640 bytes
int get_current_index()
{
    int fileIndex = 0;
    FILE* fp = fopen("currentIndex.tmp", "r");
    if (!fp) {
        fprintf(stderr, "ファイルオープン失敗: %s\n", "currentIndex.tmp");
        return 0;
    }
    fscanf(fp, "%d", &fileIndex);
    fclose(fp);
    return fileIndex;
}

int main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <ringSize> <output.pcm>\n", argv[0]);
        fprintf(stderr, "  ringSize: サイクリックバッファの最大数 (例: 610)\n");
        fprintf(stderr, "  output.pcm: 出力PCMファイル名\n");
        return 1;
    }

    // リングバッファサイズ
    int ringSize = atoi(argv[1]);
    if (ringSize <= 0) {
        fprintf(stderr, "Invalid ringSize: %d\n", ringSize);
        return 1;
    }

    // 出力ファイル名
    const char* outPcmFile = argv[2];

    // ------------------------------
    // 1) Opusデコーダの初期化
    int err;
    OpusDecoder* decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &err);
    if (err != OPUS_OK || decoder == NULL) {
        fprintf(stderr, "Failed to create Opus decoder: %s\n", opus_strerror(err));
        return 1;
    }

    // 出力ファイルを開く (連結して書き込む)
    FILE* fout = fopen(outPcmFile, "wb");
    if (!fout) {
        fprintf(stderr, "Failed to open output file: %s\n", outPcmFile);
        opus_decoder_destroy(decoder);
        return 1;
    }

    // ------------------------------
    int fileIndex_here = get_current_index();
    int fileIndex_here_last = fileIndex_here;   
    int fileIndex = fileIndex_here;
    if(fileIndex_here >= 0){
        fileIndex = fileIndex_here;
    }else{
        fileIndex = 0;
    }    
    // 2) 0 から ringSize-1 の各ファイルについてデコード
    //for (int i = 0; i < ringSize; i++) {
    int i_fileIndex_here_continuous_cnt = 0;

    while(1){
        usleep(20000); // u_sleep usec秒待つ
        //fileIndex_hereが100回連続して同じ値だったらループを抜ける
        fileIndex_here = get_current_index();
        printf("fileIndex_here = %d  i_fileIndex_here_continuous_cnt = %d\n", fileIndex_here, i_fileIndex_here_continuous_cnt);
        if(fileIndex_here == fileIndex_here_last){
            i_fileIndex_here_continuous_cnt++;
        }else{
            i_fileIndex_here_continuous_cnt = 0;
        }
        if(i_fileIndex_here_continuous_cnt >= 10){
            printf("fileIndex_here is continuous 10 times to break -----\n");
            break;
        }
        //----
        if(fileIndex_here >= 0 && fileIndex_here != fileIndex_here_last){
            fileIndex = fileIndex_here;
            fileIndex_here_last = fileIndex_here;
        }


        // 入力ファイル名を組み立て (「save_to_ring_buffer」で出力したフォルダ・ファイル名に合わせる)
        char inPath[256];
        snprintf(inPath, sizeof(inPath), "opusSampleFrames/sample-%03d.opus", fileIndex);

        // 入力ファイルを開く
        FILE* fin = fopen(inPath, "rb");
        if (!fin) {
            // 存在しないかもしれないので警告だけ出してスキップ
            fprintf(stderr, "Skipping %s (cannot open)\n", inPath);
            continue;
        }

        // ファイルサイズを取得して全部読み込む (1フレーム想定)
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

        if (readBytes != (size_t) fileSize) {
            fprintf(stderr, "Read mismatch in %s\n", inPath);
            continue;
        }

        // ------------------------------
        // 3) Opus フレームをデコード => PCM化
        //    出力は 16bit(=short) x 2ch
        short pcmOut[FRAME_SIZE * CHANNELS];  // 1フレーム用に十分なバッファ
        int frameCount = opus_decode(
            decoder,
            encodedData,
            (opus_int32) fileSize,
            pcmOut,
            FRAME_SIZE,  // フレームあたり160サンプル/チャネル
            0  // FECオプション: 0=オフ
        );
        if (frameCount < 0) {
            fprintf(stderr, "opus_decode error in %s: %s\n", inPath, opus_strerror(frameCount));
            continue;
        }

        // 通常、frameCount は 160 が返ってくるはず (20ms @8kHz)
        // 実際のサンプル数は frameCount * CHANNELS
        size_t pcmBytes = (size_t)frameCount * CHANNELS * sizeof(short);

        // ------------------------------
        // 4) PCM を出力ファイルに書き足す
        fwrite(pcmOut, 1, pcmBytes, fout);

        // ログ出力
        printf("Decoded: %s -> %ld bytes of PCM\n", inPath, pcmBytes);
    }

    // ------------------------------
    // 後処理
    fclose(fout);
    opus_decoder_destroy(decoder);
    printf("All frames decoded into %s\n", outPcmFile);

    return 0;
}
