#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __STDC_CONSTANT_MACROS

#ifdef _WIN32
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "SDL.h"
};
#elif defined(__cpluscplus)
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#endif

#define MAX_AUDIO_FRAME_SIZE 192000 // 48khz 32bit 1s 

#define OUTPUT_PCM 1 // 是否输出 PCM 文件 

#define USE_SDL 1 // 是否使用 SDL 库

//
// 缓冲区相关变量
//
static Uint8		*audio_chunk; 
static Uint32		audio_len;
static Uint8		*audio_pos;

//
// 填满stream缓冲区
// |-----------|-------------|
// chunk-------pos---len-----|
// @param: *uData, 原始数据
// @param: *stream, 指向被填充的audio buffer
// @param: len, audio buffer的长度
void fill_audio(void *uData, Uint8 *stream, int len)
{
	// SDL 2.0
	SDL_memset(stream, 0, len);
	if (audio_len == 0)
		return;

	len = len > audio_len ? audio_len : len;

	SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME); // SDL混音
	
	audio_pos += len;
	audio_len -= len;
}

int main(int argc, char* argv[])
{
	AVFormatContext		*pFormatCtx; // ffmpeg解封装结构体
	int							i, audioStream;
	AVCodecContext		*pCodecCtx; // ffmpeg编解码结构体
	AVCodec					*pCodec; // ffmpeg存储编解码器信息结构体
	AVPacket					*packet; // ffmpeg解码前数据结构体
	uint8_t						*out_buffer;
	AVFrame					*pFrame; // ffmpeg解码后数据结构体
	SDL_AudioSpec		wanted_spec; // SDL音频参数, 根据这个参数打开音频设备
	int							ret;
	uint32_t					len = 0;
	int							got_picture;
	int							index = 0;
	int64_t						in_channel_layout;
	struct SwrContext	*au_convert_ctx; // ffmpeg转换音频数据格式结构体

	FILE *pFile = nullptr;
	char url[] = "heart.mp3";

	av_register_all();
	avformat_network_init();
	pFormatCtx = avformat_alloc_context(); 

	// 打开输入
	if (avformat_open_input(&pFormatCtx, url, nullptr, nullptr) != 0)
	{
		printf("Couldn't open input stream. \n");
		return -1;
	}

	// 检索流信息
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
	{
		printf("Couldn't find stream information. \n");
		return -1;
	}

	// 填充有效信息, 0代表input
	av_dump_format(pFormatCtx, 0, url, 0);

	// 找到第一条音频流, 用audioStream记录
	audioStream = -1;
	for (i = 0; i < pFormatCtx->nb_streams; ++i)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audioStream = i;
			break;
		}
	}

	if (audioStream == -1)
	{
		printf("Can't find a audio stream.\n");
		return -1;
	}

	// 找到编解码上下文CodecContext
	pCodecCtx = avcodec_alloc_context3(nullptr);
	if (pCodecCtx == nullptr)
	{
		printf("Could not allocate AVCodecContext.\n");
		return -1;
	}
	avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[audioStream]->codecpar);

	// 找到编解码器Codec
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (pCodec == nullptr)
	{
		printf("Codec not found.\n");
		return -1;
	}

	// 打开编解码器Codec
	if (ret = avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
	{
		printf("Could not open codec.\n");
		return -1;
	}

	// 是否输出PCM文件
#if OUTPUT_PCM
	pFile = fopen("output.pcm", "wb");
#endif

	// 分配packet
	packet = reinterpret_cast<AVPacket*>(av_malloc(sizeof(AVPacket)));
	av_init_packet(packet);

	// 输出参数
	uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO; // 立体声
	int out_nb_samples = pCodecCtx->frame_size; // 每秒采样数, AAC-1024, MP3-1152
	AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16; // signed 16bits
	int out_sample_rate = 44100; // 采样率
	int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
	int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);
	out_buffer = reinterpret_cast<uint8_t *>(av_malloc(MAX_AUDIO_FRAME_SIZE * 2));
	
	// 分配frame
	pFrame = av_frame_alloc();

// 是否使用SDL
#if USE_SDL
	// SDL初始化
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
	}
	// SDL音频属性
	wanted_spec.freq = out_sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = out_channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = out_nb_samples;
	wanted_spec.callback = fill_audio; // 回调函数
	wanted_spec.userdata = pCodecCtx; // 传递给回调函数的参数

	// 打开设备
	if (SDL_OpenAudio(&wanted_spec, nullptr) < 0)
	{
		printf("Can't open audio. \n");
		return -1;
	}
#endif

	// 输入layout
	in_channel_layout = av_get_default_channel_layout(pCodecCtx->channels);
	
	// Swr
	au_convert_ctx = swr_alloc();
	au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, out_channel_layout, out_sample_fmt, out_sample_rate,
		in_channel_layout, pCodecCtx->sample_fmt, pCodecCtx->sample_rate, 0, nullptr);
	swr_init(au_convert_ctx);

	// 播放
	SDL_PauseAudio(0);

	while (av_read_frame(pFormatCtx, packet) >= 0)
	{
		if (packet->stream_index == audioStream)
		{
			if (ret = avcodec_send_packet(pCodecCtx, packet) != 0)
			{
				printf("Send packet error.\n");
				return -1;
			}
			if (ret = avcodec_receive_frame(pCodecCtx, pFrame) != 0)
			{
				printf("Receive frame error.\n");
				return -1;
			}
				
			if (ret >= 0)
			{
				swr_convert(au_convert_ctx, &out_buffer, MAX_AUDIO_FRAME_SIZE, const_cast<const uint8_t **>(reinterpret_cast<uint8_t **>(pFrame->data)), pFrame->nb_samples);
				printf("index:%5d\t pts:%lld\t packet size:%d\n", index, packet->pts, packet->size);

#if OUTPUT_PCM
				fwrite(out_buffer, 1, out_buffer_size, pFile);
#endif
				++index;
			}

#if USE_SDL
			while (audio_len > 0) // 等待播放结束
			{
				SDL_Delay(1);
			}

			audio_chunk = reinterpret_cast<Uint8 *>(out_buffer);
			audio_len = out_buffer_size;
			audio_pos = audio_chunk;
#endif
		}
		av_packet_unref(packet);
	}

	swr_free(&au_convert_ctx);

#if USE_SDL
	SDL_CloseAudio();
	SDL_Quit();
#endif

#if OUTPUT_PCM
	fclose(pFile);
#endif

	av_free(out_buffer);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);

	return 0;
}