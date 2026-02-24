#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <mmsystem.h>            // WinMM audio (link with winmm.lib)
#include <libswresample/swresample.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>

#define VIDEO_BUFFERS 2

typedef struct {
    uint8_t *data;
    int width, height;
} VideoBuffer;

VideoBuffer video_buffers[VIDEO_BUFFERS];

typedef struct {
    HWAVEOUT hwo;
    WAVEFORMATEX wfx;
    SwrContext *swr;
    int out_sample_rate;
    int out_channels;
    enum AVSampleFormat out_fmt;
    CRITICAL_SECTION lock;
    volatile LONG pending_headers;
    bool initialized;
} AudioState;

static void CALLBACK wave_out_cb(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2){
    (void)hwo;
    (void)dwParam2;

    if(uMsg != WOM_DONE) return;

    AudioState *as = (AudioState*)dwInstance;
    WAVEHDR *hdr = (WAVEHDR*)dwParam1;
    if(!as || !hdr) return;

    EnterCriticalSection(&as->lock);
    waveOutUnprepareHeader(as->hwo, hdr, sizeof(*hdr));
    if(hdr->lpData) free(hdr->lpData);
    LeaveCriticalSection(&as->lock);

    free(hdr);
    InterlockedDecrement(&as->pending_headers);
}

static bool audio_init(AudioState *as, AVCodecContext *audio_ctx, HWND hwnd){
    if(!as || !audio_ctx) return false;

    ZeroMemory(as, sizeof(*as));
    InitializeCriticalSection(&as->lock);

    as->out_sample_rate = audio_ctx->sample_rate > 0 ? audio_ctx->sample_rate : 44100;
    as->out_channels = 2;
    as->out_fmt = AV_SAMPLE_FMT_S16;

    AVChannelLayout in_ch_layout = audio_ctx->ch_layout;
    if(in_ch_layout.nb_channels == 0){
        // Fallback: assume stereo if not specified.
        av_channel_layout_default(&in_ch_layout, 2);
    }

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, as->out_channels);

    if(swr_alloc_set_opts2(
           &as->swr,
           &out_ch_layout,
           as->out_fmt,
           as->out_sample_rate,
           &in_ch_layout,
           audio_ctx->sample_fmt,
           audio_ctx->sample_rate,
           0,
           NULL) < 0 ||
       !as->swr ||
       swr_init(as->swr) < 0){
        MessageBox(hwnd, "Failed to initialize audio resampler", "Error", MB_OK);
        return false;
    }

    ZeroMemory(&as->wfx, sizeof(as->wfx));
    as->wfx.wFormatTag = WAVE_FORMAT_PCM;
    as->wfx.nChannels = (WORD)as->out_channels;
    as->wfx.nSamplesPerSec = (DWORD)as->out_sample_rate;
    as->wfx.wBitsPerSample = 16;
    as->wfx.nBlockAlign = (WORD)((as->wfx.nChannels * as->wfx.wBitsPerSample) / 8);
    as->wfx.nAvgBytesPerSec = as->wfx.nSamplesPerSec * as->wfx.nBlockAlign;

    MMRESULT mm = waveOutOpen(
        &as->hwo,
        WAVE_MAPPER,
        &as->wfx,
        (DWORD_PTR)wave_out_cb,
        (DWORD_PTR)as,
        CALLBACK_FUNCTION
    );

    if(mm != MMSYSERR_NOERROR){
        MessageBox(hwnd, "Failed to open audio device", "Error", MB_OK);
        return false;
    }

    as->initialized = true;
    return true;
}

static void audio_shutdown(AudioState *as){
    if(!as) return;

    if(as->initialized){
        DWORD start = GetTickCount();
        while(InterlockedCompareExchange(&as->pending_headers, 0, 0) != 0){
            if(GetTickCount() - start > 5000) break;
            Sleep(1);
        }
        waveOutClose(as->hwo);
    }

    if(as->swr) swr_free(&as->swr);
    DeleteCriticalSection(&as->lock);
    ZeroMemory(as, sizeof(*as));
}

static bool audio_queue_pcm(AudioState *as, const uint8_t *data, int size_bytes){
    if(!as || !as->initialized || !data || size_bytes <= 0) return false;

    WAVEHDR *hdr = (WAVEHDR*)calloc(1, sizeof(WAVEHDR));
    if(!hdr) return false;

    hdr->lpData = (LPSTR)malloc((size_t)size_bytes);
    if(!hdr->lpData){
        free(hdr);
        return false;
    }

    memcpy(hdr->lpData, data, (size_t)size_bytes);
    hdr->dwBufferLength = (DWORD)size_bytes;

    EnterCriticalSection(&as->lock);
    if(waveOutPrepareHeader(as->hwo, hdr, sizeof(*hdr)) != MMSYSERR_NOERROR){
        LeaveCriticalSection(&as->lock);
        free(hdr->lpData);
        free(hdr);
        return false;
    }
    if(waveOutWrite(as->hwo, hdr, sizeof(*hdr)) != MMSYSERR_NOERROR){
        waveOutUnprepareHeader(as->hwo, hdr, sizeof(*hdr));
        LeaveCriticalSection(&as->lock);
        free(hdr->lpData);
        free(hdr);
        return false;
    }
    LeaveCriticalSection(&as->lock);

    InterlockedIncrement(&as->pending_headers);
    return true;
}

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
    struct SwsContext *sws_ctx = NULL;
    int width = 0;
    int height = 0;

    AVCodecContext *audio_ctx = NULL;
    const AVCodec *audio_dec = NULL;
    int audio_stream_idx = -1;
    AudioState audio = {0};
    bool audio_ok = false;

    AVFrame *frame = av_frame_alloc();
    AVFrame *aframe = av_frame_alloc();
    AVPacket pkt;

    for(int i = 0; i < VIDEO_BUFFERS; i++){
        video_buffers[i].data = NULL;
        video_buffers[i].width = 0;
        video_buffers[i].height = 0;
    }

    if(!frame || !aframe){
        MessageBox(hwnd, "Failed to allocate frame(s)", "Error", MB_OK);
        goto cleanup;
    }

    if(avformat_open_input(&fmt_ctx, filename, NULL, NULL) != 0){
        MessageBox(hwnd, "Could not open MP4 file", "Error", MB_OK);
        goto cleanup;
    }

    if(avformat_find_stream_info(fmt_ctx, NULL) < 0){
        MessageBox(hwnd, "Could not read stream info", "Error", MB_OK);
        goto cleanup;
    }

    for(int i = 0; i < fmt_ctx->nb_streams; i++){
        if(fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
            video_stream_idx = i;
            break;
        }
    }

    if(video_stream_idx == -1){
        MessageBox(hwnd, "No video stream found", "Error", MB_OK);
        goto cleanup;
    }

    for(int i = 0; i < fmt_ctx->nb_streams; i++){
        if(fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
            audio_stream_idx = i;
            break;
        }
    }

    video_dec = avcodec_find_decoder(
        fmt_ctx->streams[video_stream_idx]->codecpar->codec_id
    );
    if(!video_dec){
        MessageBox(hwnd, "No suitable video decoder found", "Error", MB_OK);
        goto cleanup;
    }

    video_ctx = avcodec_alloc_context3(video_dec);
    if(!video_ctx){
        MessageBox(hwnd, "Failed to allocate video decoder context", "Error", MB_OK);
        goto cleanup;
    }
    avcodec_parameters_to_context(
        video_ctx,
        fmt_ctx->streams[video_stream_idx]->codecpar
    );

    if(avcodec_open2(video_ctx, video_dec, NULL) < 0){
        MessageBox(hwnd, "Failed to open decoder", "Error", MB_OK);
        goto cleanup;
    }

    if(audio_stream_idx != -1){
        audio_dec = avcodec_find_decoder(
            fmt_ctx->streams[audio_stream_idx]->codecpar->codec_id
        );
        if(audio_dec){
            audio_ctx = avcodec_alloc_context3(audio_dec);
            avcodec_parameters_to_context(
                audio_ctx,
                fmt_ctx->streams[audio_stream_idx]->codecpar
            );

            if(avcodec_open2(audio_ctx, audio_dec, NULL) >= 0){
                audio_ok = audio_init(&audio, audio_ctx, hwnd);
            }
        }
    }

    width = video_ctx->width;
    height = video_ctx->height;

    for(int i = 0; i < VIDEO_BUFFERS; i++){
        int buf_size = av_image_get_buffer_size(
            AV_PIX_FMT_RGB24,
            width,
            height,
            1
        );

        video_buffers[i].data = malloc(buf_size);
        if(!video_buffers[i].data){
            MessageBox(hwnd, "Failed to allocate video buffer", "Error", MB_OK);
            goto cleanup;
        }
        video_buffers[i].width = width;
        video_buffers[i].height = height;
    }

    sws_ctx = sws_getContext(
        width, height, video_ctx->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if(!sws_ctx){
        MessageBox(hwnd, "Failed to initialize scaler", "Error", MB_OK);
        goto cleanup;
    }

    int current_buf = 0;

    while(av_read_frame(fmt_ctx, &pkt) >= 0){

        MSG msg;
        while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)){
            if(msg.message == WM_QUIT){
                av_packet_unref(&pkt);
                goto cleanup;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

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
        }else if(audio_ok && pkt.stream_index == audio_stream_idx){

            avcodec_send_packet(audio_ctx, &pkt);

            while(avcodec_receive_frame(audio_ctx, aframe) == 0){
                int out_nb_samples = av_rescale_rnd(
                    swr_get_delay(audio.swr, audio_ctx->sample_rate) + aframe->nb_samples,
                    audio.out_sample_rate,
                    audio_ctx->sample_rate,
                    AV_ROUND_UP
                );

                int out_linesize = 0;
                uint8_t *out_buf = NULL;
                int rc = av_samples_alloc(
                    &out_buf,
                    &out_linesize,
                    audio.out_channels,
                    out_nb_samples,
                    audio.out_fmt,
                    1
                );
                if(rc < 0 || !out_buf){
                    break;
                }

                uint8_t *out_data[1] = { out_buf };
                int converted = swr_convert(
                    audio.swr,
                    out_data,
                    out_nb_samples,
                    (const uint8_t**)aframe->data,
                    aframe->nb_samples
                );

                if(converted > 0){
                    int bytes = av_samples_get_buffer_size(
                        NULL,
                        audio.out_channels,
                        converted,
                        audio.out_fmt,
                        1
                    );
                    if(bytes > 0) audio_queue_pcm(&audio, out_buf, bytes);
                }

                av_freep(&out_buf);
            }
        }

        av_packet_unref(&pkt);
    }

cleanup:

    for(int i = 0; i < VIDEO_BUFFERS; i++)
        if(video_buffers[i].data) free(video_buffers[i].data);

    if(sws_ctx) sws_freeContext(sws_ctx);
    if(frame) av_frame_free(&frame);
    if(video_ctx) avcodec_free_context(&video_ctx);
    if(aframe) av_frame_free(&aframe);
    if(audio_ok) audio_shutdown(&audio);
    if(audio_ctx) avcodec_free_context(&audio_ctx);
    if(fmt_ctx) avformat_close_input(&fmt_ctx);
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