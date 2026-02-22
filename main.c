#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <mmsystem.h> // for audio output

// ---- Global config ----
#define VIDEO_BUFFERS 2
#define AUDIO_BUFFER_SIZE 192000  // ~1 sec at 48kHz, stereo

// ---- Simple video buffer struct ----
typedef struct {
    uint8_t *data;
    int width, height;
    enum AVPixelFormat fmt;
} VideoBuffer;

VideoBuffer video_buffers[VIDEO_BUFFERS];
uint8_t audio_buffer[AUDIO_BUFFER_SIZE]; // simple audio buffer

// ---- Forward declarations ----
void play_video(const char *filename, HWND hwnd);

// ---- Win32 window callback ----
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    if(msg==WM_DESTROY){ PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main(int argc, char *argv[]){
    if(argc < 2){
        printf("Usage: %s <fr.mp4>\n", argv[0]);
        return 0;
    }

    avformat_network_init(); // only needed if streaming

    // Create simple window
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MP4PlayerWindow";
    RegisterClass(&wc);

    HWND hwnd = CreateWindow("MP4PlayerWindow","MP4 Player",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT,CW_USEDEFAULT,
                             1280,720, NULL,NULL, wc.hInstance,NULL);

    play_video(argv[1], hwnd);

    return 0;
}

void play_video(const char *filename, HWND hwnd){
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *video_ctx = NULL, *audio_ctx = NULL;
    AVCodec *video_dec = NULL, *audio_dec = NULL;
    int video_stream_idx=-1, audio_stream_idx=-1;
    AVFrame *frame = av_frame_alloc();
    AVPacket pkt;

    // Open file
    if(avformat_open_input(&fmt_ctx, filename, NULL, NULL)!=0){
        printf("Error opening file\n"); return;
    }
    avformat_find_stream_info(fmt_ctx, NULL);

    // Find streams and open codecs
    for(int i=0;i<fmt_ctx->nb_streams;i++){
        if(fmt_ctx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && video_stream_idx<0){
            video_stream_idx=i;
            video_dec=avcodec_find_decoder(fmt_ctx->streams[i]->codecpar->codec_id);
            video_ctx=avcodec_alloc_context3(video_dec);
            avcodec_parameters_to_context(video_ctx, fmt_ctx->streams[i]->codecpar);
            avcodec_open2(video_ctx, video_dec, NULL);
        }
        else if(fmt_ctx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && audio_stream_idx<0){
            audio_stream_idx=i;
            audio_dec=avcodec_find_decoder(fmt_ctx->streams[i]->codecpar->codec_id);
            audio_ctx=avcodec_alloc_context3(audio_dec);
            avcodec_parameters_to_context(audio_ctx, fmt_ctx->streams[i]->codecpar);
            avcodec_open2(audio_ctx, audio_dec, NULL);
        }
    }

    int width=video_ctx->width;
    int height=video_ctx->height;

    // Allocate minimal video buffers
    for(int i=0;i<VIDEO_BUFFERS;i++){
        int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
        video_buffers[i].data = (uint8_t*)malloc(buf_size);
        video_buffers[i].width = width;
        video_buffers[i].height = height;
        video_buffers[i].fmt = AV_PIX_FMT_RGB24;
    }

    // Initialize scaler
    struct SwsContext *sws_ctx = sws_getContext(width,height,video_ctx->pix_fmt,
                                                width,height,AV_PIX_FMT_RGB24,
                                                SWS_BILINEAR, NULL,NULL,NULL);

    int current_buf=0;
    while(av_read_frame(fmt_ctx,&pkt)>=0){
        if(pkt.stream_index==video_stream_idx){
            avcodec_send_packet(video_ctx,&pkt);
            while(avcodec_receive_frame(video_ctx,frame)==0){
                // Prepare destination pointers
                uint8_t *dst_data[4];
                int dst_linesize[4];
                av_image_fill_arrays(dst_data, dst_linesize, video_buffers[current_buf].data,
                                     AV_PIX_FMT_RGB24, width, height, 1);

                sws_scale(sws_ctx,
                          (const uint8_t* const*)frame->data,
                          frame->linesize,
                          0,
                          height,
                          dst_data,
                          dst_linesize);

                // Render video
                HDC hdc = GetDC(hwnd);
                BITMAPINFO bmi = {0};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = width;
                bmi.bmiHeader.biHeight = -height;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 24;
                bmi.bmiHeader.biCompression = BI_RGB;
                StretchDIBits(hdc,0,0,width,height,
                              0,0,width,height,
                              video_buffers[current_buf].data,&bmi,DIB_RGB_COLORS,SRCCOPY);
                ReleaseDC(hwnd,hdc);

                current_buf = (current_buf+1)%VIDEO_BUFFERS;
                Sleep(16); // ~60fps
            }
        }
        else if(pkt.stream_index==audio_stream_idx){
            avcodec_send_packet(audio_ctx,&pkt);
            while(avcodec_receive_frame(audio_ctx,frame)==0){
                // Audio placeholder
                // Needs SwrContext to convert to 16-bit stereo PCM
            }
        }
        av_packet_unref(&pkt);
    }

    // Free resources
    for(int i=0;i<VIDEO_BUFFERS;i++)
        free(video_buffers[i].data);
    av_frame_free(&frame);
    avcodec_free_context(&video_ctx);
    avcodec_free_context(&audio_ctx);
    avformat_close_input(&fmt_ctx);
    sws_freeContext(sws_ctx);
}