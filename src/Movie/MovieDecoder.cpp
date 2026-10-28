#include "MovieDecoder.h"

#if BF2142_HAS_MOVIES
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace dice::hfe::movie {

#if BF2142_HAS_MOVIES

struct MovieDecoder::Impl {
	AVFormatContext* format = nullptr;
	AVCodecContext* video = nullptr;
	AVCodecContext* audio = nullptr;
	int videoStream = -1, audioStream = -1;
	SwsContext* scaler = nullptr;
	SwrContext* resampler = nullptr;
	AVPacket* packet = nullptr;
	AVFrame* frame = nullptr;
	std::vector<float> samples;
	bool ended = false;   // no more packets
	bool flushed = false; // the decoders were told so

	~Impl()
	{
		sws_freeContext(scaler);
		swr_free(&resampler);
		av_packet_free(&packet);
		av_frame_free(&frame);
		avcodec_free_context(&video);
		avcodec_free_context(&audio);
		avformat_close_input(&format);
	}

	static AVCodecContext* openCodec(AVFormatContext* format, int stream)
	{
		const AVCodecParameters* parameters = format->streams[stream]->codecpar;
		const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
		if (!codec) {
			return nullptr;
		}
		AVCodecContext* context = avcodec_alloc_context3(codec);
		if (!context || avcodec_parameters_to_context(context, parameters) < 0 || avcodec_open2(context, codec, nullptr) < 0) {
			avcodec_free_context(&context);
			return nullptr;
		}
		return context;
	}

	void takeAudioFrames()
	{
		while (audio && avcodec_receive_frame(audio, frame) == 0) {
			if (!resampler) {
				AVChannelLayout stereo;
				av_channel_layout_default(&stereo, 2);
				if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT, audio->sample_rate, &frame->ch_layout,
						static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0
					|| swr_init(resampler) < 0) {
					swr_free(&resampler);
					av_frame_unref(frame);
					return;
				}
			}
			const int count = swr_get_out_samples(resampler, frame->nb_samples);
			const size_t at = samples.size();
			samples.resize(at + static_cast<size_t>(count) * 2);
			uint8_t* out[1] = {reinterpret_cast<uint8_t*>(samples.data() + at)};
			const int made = swr_convert(resampler, out, count, const_cast<const uint8_t**>(frame->extended_data),
				frame->nb_samples);
			samples.resize(at + static_cast<size_t>(made > 0 ? made : 0) * 2);
			av_frame_unref(frame);
		}
	}
};

MovieDecoder::MovieDecoder() = default;
MovieDecoder::~MovieDecoder() = default;

bool MovieDecoder::available() { return true; }

bool MovieDecoder::open(const std::string& path, std::string* error)
{
	close();
	auto impl = std::make_unique<Impl>();
	if (avformat_open_input(&impl->format, path.c_str(), nullptr, nullptr) < 0) {
		if (error) {
			*error = "cannot open " + path;
		}
		return false;
	}
	if (avformat_find_stream_info(impl->format, nullptr) < 0) {
		if (error) {
			*error = "no streams in " + path;
		}
		return false;
	}
	impl->videoStream = av_find_best_stream(impl->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (impl->videoStream < 0 || !(impl->video = Impl::openCodec(impl->format, impl->videoStream))) {
		if (error) {
			*error = "no video decoder for " + path;
		}
		return false;
	}
	impl->audioStream = av_find_best_stream(impl->format, AVMEDIA_TYPE_AUDIO, -1, impl->videoStream, nullptr, 0);
	if (impl->audioStream >= 0) {
		impl->audio = Impl::openCodec(impl->format, impl->audioStream);
		if (!impl->audio) {
			impl->audioStream = -1; // the picture without the sound
		}
	}
	impl->packet = av_packet_alloc();
	impl->frame = av_frame_alloc();
	m_impl = std::move(impl);
	return true;
}

void MovieDecoder::close() { m_impl.reset(); }
bool MovieDecoder::isOpen() const { return m_impl != nullptr; }
int MovieDecoder::width() const { return m_impl ? m_impl->video->width : 0; }
int MovieDecoder::height() const { return m_impl ? m_impl->video->height : 0; }

double MovieDecoder::frameRate() const
{
	if (!m_impl) {
		return 0;
	}
	const AVRational rate = m_impl->format->streams[m_impl->videoStream]->avg_frame_rate;
	return rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30.0;
}

double MovieDecoder::duration() const
{
	return m_impl && m_impl->format->duration > 0 ? static_cast<double>(m_impl->format->duration) / AV_TIME_BASE : 0.0;
}

bool MovieDecoder::hasAudio() const { return m_impl && m_impl->audio; }
int MovieDecoder::sampleRate() const { return hasAudio() ? m_impl->audio->sample_rate : 0; }

bool MovieDecoder::nextFrame(VideoFrame& out)
{
	if (!m_impl) {
		return false;
	}
	Impl& d = *m_impl;
	for (;;) {
		const int got = avcodec_receive_frame(d.video, d.frame);
		if (got == 0) {
			const int w = d.frame->width, h = d.frame->height;
			d.scaler = sws_getCachedContext(d.scaler, w, h, static_cast<AVPixelFormat>(d.frame->format), w, h,
				AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
			out.width = w;
			out.height = h;
			out.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
			uint8_t* planes[1] = {out.rgba.data()};
			const int strides[1] = {w * 4};
			sws_scale(d.scaler, d.frame->data, d.frame->linesize, 0, h, planes, strides);
			const AVRational base = d.format->streams[d.videoStream]->time_base;
			const int64_t pts = d.frame->best_effort_timestamp != AV_NOPTS_VALUE ? d.frame->best_effort_timestamp : d.frame->pts;
			out.time = pts != AV_NOPTS_VALUE ? static_cast<double>(pts) * av_q2d(base) : out.time + 1.0 / frameRate();
			av_frame_unref(d.frame);
			return true;
		}
		if (got == AVERROR_EOF || (d.flushed && got == AVERROR(EAGAIN))) {
			return false;
		}
		// The decoder wants more: the next packets (audio ones go to the
		// audio decoder on the way).
		if (d.ended) {
			if (!d.flushed) {
				avcodec_send_packet(d.video, nullptr);
				if (d.audio) {
					avcodec_send_packet(d.audio, nullptr);
					d.takeAudioFrames();
				}
				d.flushed = true;
			}
			continue;
		}
		if (av_read_frame(d.format, d.packet) < 0) {
			d.ended = true;
			continue;
		}
		if (d.packet->stream_index == d.videoStream) {
			avcodec_send_packet(d.video, d.packet);
		} else if (d.audio && d.packet->stream_index == d.audioStream) {
			if (avcodec_send_packet(d.audio, d.packet) == 0) {
				d.takeAudioFrames();
			}
		}
		av_packet_unref(d.packet);
	}
}

std::vector<float> MovieDecoder::takeAudio()
{
	std::vector<float> out;
	if (m_impl) {
		out.swap(m_impl->samples);
	}
	return out;
}

#else // without FFmpeg

struct MovieDecoder::Impl {};

MovieDecoder::MovieDecoder() = default;
MovieDecoder::~MovieDecoder() = default;
bool MovieDecoder::available() { return false; }
bool MovieDecoder::open(const std::string& path, std::string* error)
{
	if (error) {
		*error = "built without FFmpeg: cannot play " + path;
	}
	return false;
}
void MovieDecoder::close() {}
bool MovieDecoder::isOpen() const { return false; }
int MovieDecoder::width() const { return 0; }
int MovieDecoder::height() const { return 0; }
double MovieDecoder::frameRate() const { return 0; }
double MovieDecoder::duration() const { return 0; }
bool MovieDecoder::hasAudio() const { return false; }
int MovieDecoder::sampleRate() const { return 0; }
bool MovieDecoder::nextFrame(VideoFrame&) { return false; }
std::vector<float> MovieDecoder::takeAudio() { return {}; }

#endif

} // namespace dice::hfe::movie
