#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define VIDEO_BUFFERS 2

typedef struct {
    uint8_t *data;
    int width, height;
} VideoBuffer;

VideoBuffer video_buffers[VIDEO_BUFFERS];

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    if(msg == WM_DESTROY){
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void play_video(const char *filename, HWND hwnd){

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *video_ctx = NULL;
    const AVCodec *video_dec = NULL;
    int video_stream_idx = -1;

    AVFrame *frame = av_frame_alloc();
    AVPacket pkt;

    if(avformat_open_input(&fmt_ctx, filename, NULL, NULL) != 0){
        MessageBox(hwnd, "Could not open MP4 file", "Error", MB_OK);
        return;
    }

    if(avformat_find_stream_info(fmt_ctx, NULL) < 0){
        MessageBox(hwnd, "Could not read stream info", "Error", MB_OK);
        return;
    }

    for(int i = 0; i < fmt_ctx->nb_streams; i++){
        if(fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
            video_stream_idx = i;
            break;
        }
    }

    if(video_stream_idx == -1){
        MessageBox(hwnd, "No video stream found", "Error", MB_OK);
        return;
    }

    video_dec = avcodec_find_decoder(
        fmt_ctx->streams[video_stream_idx]->codecpar->codec_id
    );

    video_ctx = avcodec_alloc_context3(video_dec);
    avcodec_parameters_to_context(
        video_ctx,
        fmt_ctx->streams[video_stream_idx]->codecpar
    );

    if(avcodec_open2(video_ctx, video_dec, NULL) < 0){
        MessageBox(hwnd, "Failed to open decoder", "Error", MB_OK);
        return;
    }

    int width = video_ctx->width;
    int height = video_ctx->height;

    for(int i = 0; i < VIDEO_BUFFERS; i++){
        int buf_size = av_image_get_buffer_size(
            AV_PIX_FMT_RGB24,
            width,
            height,
            1
        );

        video_buffers[i].data = malloc(buf_size);
        video_buffers[i].width = width;
        video_buffers[i].height = height;
    }

    struct SwsContext *sws_ctx = sws_getContext(
        width, height, video_ctx->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    int current_buf = 0;

    while(av_read_frame(fmt_ctx, &pkt) >= 0){

        if(pkt.stream_index == video_stream_idx){

            avcodec_send_packet(video_ctx, &pkt);

            while(avcodec_receive_frame(video_ctx, frame) == 0){

                uint8_t *dst_data[4];
                int dst_linesize[4];

                av_image_fill_arrays(
                    dst_data,
                    dst_linesize,
                    video_buffers[current_buf].data,
                    AV_PIX_FMT_RGB24,
                    width,
                    height,
                    1
                );

                sws_scale(
                    sws_ctx,
                    (const uint8_t* const*)frame->data,
                    frame->linesize,
                    0,
                    height,
                    dst_data,
                    dst_linesize
                );

                HDC hdc = GetDC(hwnd);

                BITMAPINFO bmi = {0};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = width;
                bmi.bmiHeader.biHeight = -height;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 24;
                bmi.bmiHeader.biCompression = BI_RGB;

                StretchDIBits(
                    hdc,
                    0, 0, width, height,
                    0, 0, width, height,
                    video_buffers[current_buf].data,
                    &bmi,
                    DIB_RGB_COLORS,
                    SRCCOPY
                );

                ReleaseDC(hwnd, hdc);

                current_buf = (current_buf + 1) % VIDEO_BUFFERS;

                Sleep(16);
            }
        }

        av_packet_unref(&pkt);
    }

    for(int i = 0; i < VIDEO_BUFFERS; i++)
        free(video_buffers[i].data);

    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    avcodec_free_context(&video_ctx);
    avformat_close_input(&fmt_ctx);
}

int main(){

    avformat_network_init();

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MP4PlayerWindow";
    RegisterClass(&wc);

    HWND hwnd = CreateWindow(
        "MP4PlayerWindow",
        "MP4 Player",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 720,
        NULL, NULL,
        wc.hInstance,
        NULL
    );

    if(!hwnd){
        MessageBox(NULL, "Window creation failed", "Error", MB_OK);
        return 1;
    }

    // Always loads this file
    play_video("fr.mp4", hwnd);

    // Keep window alive
    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}