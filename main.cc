#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/gil/extension/io/png_io.hpp>
#include <boost/thread.hpp>

extern "C"
{
	#include <fftw3.h>
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libavutil/imgutils.h>
}

struct Stream
{
	AVFrame *frame, *next_frame;
	AVCodec *codec;
	AVCodecContext *codec_context;
	AVStream *stream;
	uint8_t *previous_image;
	bool skip;
	float *signal_in;
	fftwf_complex *signal_out;
	fftwf_plan plan;

	boost::posix_time::time_duration
	date(long correction) const
	{
		return boost::posix_time::seconds(long(stream->cur_dts * stream->time_base.num / stream->time_base.den + correction));
	}
};

static std::vector<Stream> _streams;
static Stream *_stream;
static bool _good;
static boost::condition_variable _condition;
static boost::mutex _mutex;

static void
decode(const char *url)
{
	int status;
	AVFormatContext *format_context = 0;
	status = avformat_open_input(&format_context, url, 0, 0);
	assert(!status);
	assert(format_context);
	status = avformat_find_stream_info(format_context, 0);
	assert(status >= 0);

	// -- initialisation des flux

	_streams.resize(format_context->nb_streams);
	for(size_t i = 0; i < _streams.size(); ++i)
	{
		Stream &s = _streams[i];
		s.stream = format_context->streams[i];
		s.codec_context = s.stream->codec;
		assert(s.codec_context);
		s.codec = avcodec_find_decoder(s.codec_context->codec_id);
		assert(s.codec);
		if(s.codec->capabilities & CODEC_CAP_TRUNCATED)
		{
			s.codec_context->flags |= CODEC_FLAG_TRUNCATED;
		}
		status = avcodec_open2(s.codec_context, s.codec, 0);
		assert(status >= 0);
		s.frame = avcodec_alloc_frame();
		assert(s.frame);
		s.next_frame = avcodec_alloc_frame();
		assert(s.next_frame);
		avcodec_get_frame_defaults(s.next_frame);
		s.previous_image = 0;
		s.signal_in = 0;
		s.signal_out = 0;
		s.skip = false;
	}

	// -- décodage de chaque trame

	AVPacket packet;
	av_init_packet(&packet);
	packet.data = 0;
	packet.size = 0;
	for(;;)
	{
		while(packet.size > 0)
		{
			Stream &s = _streams[packet.stream_index];
			static int (*const functions[])(AVCodecContext *, AVFrame *, int *, AVPacket *) = {avcodec_decode_video2, avcodec_decode_audio4};
			int got_frame;
			const int count = functions[s.codec_context->codec_type](s.codec_context, s.next_frame, &got_frame, &packet);
			assert(count >= 0);
			packet.size -= count;
			packet.data += count;
			if(got_frame)
			{
				boost::mutex::scoped_lock lock(_mutex);
				std::swap(s.frame, s.next_frame);
				avcodec_get_frame_defaults(s.next_frame);
				_stream = &s;
				_condition.notify_one();
			}
		}
		status = av_read_frame(format_context, &packet);
		if(status)
		{
			break;
		}
	}

	// -- la vidéo est terminée

	boost::mutex::scoped_lock lock(_mutex);
	_good = false;
	_condition.notify_one();
	for(size_t i = 0; i < format_context->nb_streams; ++i)
	{
		Stream &s = _streams[i];
		av_free(s.frame);
		av_free(s.next_frame);
		avcodec_close(s.codec_context);
		if(s.previous_image)
		{
			delete[] s.previous_image;
		}
		if(s.signal_in)
		{
			fftwf_destroy_plan(s.plan);
			fftwf_free(s.signal_in);
			fftwf_free(s.signal_out);
		}
	}
	avformat_close_input(&format_context);
	av_free(format_context);
}

static float
sample(AVFrame *frame, size_t i)
{
	switch(frame->format)
	{
		case AV_SAMPLE_FMT_U8: return float(frame->data[0][i]) / UCHAR_MAX;
		case AV_SAMPLE_FMT_S16: return float(((int16_t *)(frame->data[0]))[i]) / SHRT_MAX;
		case AV_SAMPLE_FMT_S32: return float(((int32_t *)(frame->data[0]))[i]) / float(INT_MAX);
		case AV_SAMPLE_FMT_FLT: return ((float *)(frame->data[0]))[i];
		case AV_SAMPLE_FMT_DBL: return float(((double *)(frame->data[0]))[i]);
		default: throw frame;
	}
}

int
main(int file_count, char **files)
{
	const int color_threshold = atoi(getenv("color_threshold"));
	const size_t pixels_threshold = atoi(getenv("pixels_threshold"));
	const float audio_threshold = float(atof(getenv("audio_threshold")));
	const int counter_max = atoi(getenv("alert_threshold"));
	const int timeout_max = atoi(getenv("relaxation_time"));
	boost::gil::gray8_image_t mask;
	png_read_and_convert_image(getenv("mask"), mask);
	av_register_all();
	boost::mutex::scoped_lock lock(_mutex);
	for(int i = 1; i < file_count; ++i)
	{
		_good = true;
		boost::thread decoder(boost::bind(decode, files[i])); // décode la vidéo en tâche de fond
		long counter = counter_max;
		long timeout = timeout_max;
		for(;;)
		{
			_stream = 0;
			while(!_stream) // attend la prochaine trame
			{
				_condition.wait(lock); // le décodeur peut entrer en section critique pendant cette attente
				if(!_good)
				{
					goto next_file;
				}
			}
			Stream &s = *_stream; // on a décodé une trame d'un flux
			bool test = false; // sera vrai si on détecte du mouvement
			switch(s.codec_context->codec_type) // différents traitements en fonction du type de données
			{
				case AVMEDIA_TYPE_VIDEO:
				{
					if(s.skip) // saute une image pour aller plus vite
					{
						s.skip = false;
						break;
					}
					if(s.previous_image)
					{
						size_t n = 0; // sera le nombre de pixels changés depuis l'image précédente
						for(int v = 0; v < s.frame->height; v += 2) // saute une ligne pour aller plus vite
						{
							for(int u = 0; u < s.frame->width; ++u)
							{
								if(view(mask)(u, v) && std::abs(long(s.frame->data[0][v * s.frame->linesize[0] + u]) - long(s.previous_image[v * s.frame->width + u])) > color_threshold) // ce pixel a beaucoup changé
								{
									++n;
								}
							}
						}
						test = n > pixels_threshold;
					}
					else
					{
						s.previous_image = new uint8_t[s.frame->width * s.frame->height];
					}
					av_image_copy_plane(s.previous_image, s.frame->width, s.frame->data[0], s.frame->linesize[0], s.frame->width, s.frame->height);
					s.skip = true;
					break;
				}
				case AVMEDIA_TYPE_AUDIO:
				{
					if(s.signal_in)
					{
						for(int i = 0; i < s.frame->nb_samples; ++i)
						{
							s.signal_in[i] = sample(s.frame, i);
						}
						fftwf_execute(s.plan);
						float power = 0; // sera l'énergie du tronçon de signal sonore
						for(int i = 0; i < s.frame->nb_samples / 16; ++i) // on ne considère que les basses fréquences (voix, aboiements)
						{
							power += s.signal_out[i][0] * s.signal_out[i][0] + s.signal_out[i][1] * s.signal_out[i][1];
						}
						test = power > audio_threshold;
					}
					else
					{
						s.signal_in = fftwf_alloc_real(s.frame->nb_samples);
						s.signal_out = fftwf_alloc_complex(s.frame->nb_samples / 2 + 1);
						s.plan = fftwf_plan_dft_r2c_1d(s.frame->nb_samples, s.signal_in, s.signal_out, FFTW_ESTIMATE);
					}
					break;
				}
				default:
				{
					goto next_frame;
				}
			}
			if(test) // on vient de détecter du mouvement
			{
				if(counter) // le mouvement n'est pas encore assez gros
				{
					if(!--counter) // le mouvement devient assez gros
					{
						std::cout << files[i] << ' ' << s.date(-2); // c'est un début d'événement, on note la date
					}
				}
				timeout = timeout_max; // on observe peut-être un événement, donc méfiance
			}
			if(timeout) // on est en train d'observer un mouvement
			{
				if(!--timeout) // rien ne bouge depuis assez longtemps
				{
					if(!counter) // pourtant, on avait un gros mouvement
					{
						std::cout << ' ' << s.date(0) << std::endl; // c'est une fin d'événement, on note la date
					}
					counter = counter_max; // l'alerte est levée
				}
			}
		next_frame:
			;
		}
	next_file:
		decoder.join();
	}
}
