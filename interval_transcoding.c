#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <assert.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include "cmdline.h"

#include "dump.c"
#define log(level, fmt_n_args...) av_log(NULL, AV_LOG_##level, fmt_n_args)

// rework places with these defines when need support of other containers

struct transcoder {
    struct args args;
    int video_ind;
    AVFormatContext *avInputFmtCtx;
    AVFormatContext *avOutputFmtCtx;
    AVCodecContext *avInputVideoDecoderCtx;
    AVCodecContext *avOutputVideoEncoderCtx;
    AVPacket read_pkt;
    int decode_ret;
    int got_picture;
    AVFrame *frame;
    DECLARE_ALIGNED(16, uint8_t, decbuf)[1024 * 1024];
    DECLARE_ALIGNED(16, uint8_t, encbuf)[1024 * 1024];
    int finished;
};
typedef struct transcoder Transcoder;

int tc_input_open(Transcoder *tc);
int tc_output_open(Transcoder *tc);
int tc_decoder_open(Transcoder *tc);
int tc_encoder_open(Transcoder *tc);
static int tc_prepare(Transcoder *tc);
static int tc_processing_loop(Transcoder *tc);
static int tc_process_frame(Transcoder *tc);
static void tc_destroy(Transcoder *tc);

int main(int argc, char *argv[]) {
    int r;
    /* Global init */
    av_register_all();
    /* END Global init */

    Transcoder *tc = calloc(1, sizeof(*tc));
    if (!tc) {
        log(ERROR, "cant alloc transcoder\n");
        return 1;
    }

    r = cmdline_parser(argc, argv, &tc->args);
    if (r) {
        cmdline_parser_print_help();
        return 1;
    }
    assert((tc->args.loglevel_arg >= 0) && (tc->args.loglevel_arg <= 6));
    av_log_set_level(tc->args.loglevel_arg * 8);

    r = tc_prepare(tc);
    if (r) {
        log(ERROR, "transcoder prepare fail\n");
        return 1;
    }

    r = tc_processing_loop(tc);
    if (r) {
        log(ERROR, "processing fail\n");
        return 1;
    }

    tc_destroy(tc);
    free(tc);

    return 0;
}

static int tc_prepare(Transcoder *tc) {
    int r;

    tc->video_ind = -1;

    r = tc_input_open(tc);
    if (r)
        return r;

    r = tc_output_open(tc);
    if (r)
        return r;

    r = tc_decoder_open(tc);
    if (r)
        return r;

    r = tc_encoder_open(tc);
    if (r)
        return r;

    av_dump_format(tc->avOutputFmtCtx, 0, tc->avOutputFmtCtx->filename, 1);

    r = avformat_write_header(tc->avOutputFmtCtx, NULL);
    if (r) {
        log(ERROR, "Could not write header for output file");
        return 1;
    }

    av_dump_format(tc->avOutputFmtCtx, 0, tc->avOutputFmtCtx->filename, 1);
    return 0;
}

static int tc_packet_write_and_free(Transcoder *tc, AVPacket *pkt) {
    int r;
    //r = av_write_frame(tc->avOutputFmtCtx, pkt);
    r = tc->avOutputFmtCtx->oformat->write_packet(tc->avOutputFmtCtx, pkt);
    av_free_packet(pkt);
    if (r) {
        log(ERROR, "packet write fail\n");
        return 1;
    }
    return 0;
}

static int tc_processing_loop(Transcoder *tc) {
    int r = 0;
    while ( ! tc->finished) {
        r = tc_process_frame(tc);
    }
    log(INFO, "Out of transcoding loop\n");
    return r;
}

static int tc_read_frame(Transcoder *tc) {
    int r;
    r = av_read_frame(tc->avInputFmtCtx, &tc->read_pkt);
    if (r == AVERROR_EOF) {
        log(INFO, "EOF reached\n");
        tc->finished = 1;
        return 1;
    }
    if (r) {
        log(ERROR, "av_read_frame ret %d\n", r);
        return r;
    }
    log(DEBUG, "read frame of stream %d with pts %"PRId64", dts %"PRId64"\n",
            tc->read_pkt.stream_index, tc->read_pkt.pts, tc->read_pkt.dts);
    log(DEBUG, "is %skeyframe\n", tc->read_pkt.flags & AV_PKT_FLAG_KEY ? "" : "not ");

    if ((tc->read_pkt.pts == AV_NOPTS_VALUE) || (tc->read_pkt.dts == AV_NOPTS_VALUE)) {
        log(ERROR, "non-timestamped incoming packet, discarding\n");
        //av_free_packet(&tc->read_pkt);
        //return 1;
        tc->read_pkt.pts = tc->read_pkt.dts = 0;
    }
    if(tc->read_pkt.pts < tc->read_pkt.dts) {
        log(ERROR, "wrongly timestamped incoming packet: pts %"PRId64", dts %"PRId64", discarding\n",
                tc->read_pkt.pts, tc->read_pkt.dts);
        av_free_packet(&tc->read_pkt);
        return 1;
    }
    return 0;
}

static int tc_decode_frame(Transcoder *tc) {
        tc->frame = avcodec_alloc_frame();
        assert(tc->frame);
        tc->decode_ret = avcodec_decode_video2(tc->avInputVideoDecoderCtx, tc->frame, &tc->got_picture, &tc->read_pkt);
        if (tc->decode_ret < 0) {
            log(WARNING, "decode fail\n");
            av_free(tc->frame);
        }
        return tc->decode_ret;
}

static int tc_straight_write(Transcoder *tc) {
    int r;
    log(DEBUG, "straight writing of read frame\n");
    tc->read_pkt.pts = av_rescale_q(tc->read_pkt.pts,
            tc->avInputFmtCtx->streams[tc->read_pkt.stream_index]->time_base,
            tc->avOutputFmtCtx->streams[tc->read_pkt.stream_index]->time_base);
    tc->read_pkt.dts = av_rescale_q(tc->read_pkt.dts,
            tc->avInputFmtCtx->streams[tc->read_pkt.stream_index]->time_base,
            tc->avOutputFmtCtx->streams[tc->read_pkt.stream_index]->time_base);
    log(DEBUG, "writing with rescaled pts %"PRId64", dts %"PRId64"\n",
            tc->read_pkt.pts, tc->read_pkt.dts);
    r = tc_packet_write_and_free(tc, &tc->read_pkt);
    if (r)
        return r;
    return 0;
}

static int tc_encode_write_frame(Transcoder *tc) {
    int r;
    int encbuf_size = sizeof(tc->encbuf);
    log(DEBUG, "gonna encode\n");
    if ((tc->decode_ret < 0) || !tc->got_picture) {
        av_free_packet(&tc->read_pkt);
        log(DEBUG, "should encode, but got no picture\n");
        return 1;
    }

#ifdef LIBAV
    tc->frame->pts = tc->frame->pkt_pts;
#else
    tc->frame->pts = tc->frame->best_effort_timestamp;
#endif
    log(DEBUG, "decoded frame pts: %"PRId64"\n", tc->frame->pts);
    tc->frame->pts = av_rescale_q(tc->frame->pts,
            tc->avInputFmtCtx->streams[tc->read_pkt.stream_index]->time_base,
            tc->avInputVideoDecoderCtx->time_base);
    log(DEBUG, "frame pts, rescaled after decoding: %"PRId64"\n", tc->frame->pts);

    tc->frame->pict_type = 0;
    r = avcodec_encode_video(tc->avOutputVideoEncoderCtx, tc->encbuf, encbuf_size, tc->frame);
    if (r < 0) {
        log(ERROR, "encode video fail\n");
        return r;
    }
    if (r > 0) {
        encbuf_size = r;
        AVPacket x_pkt; // transcoded packet
        av_init_packet(&x_pkt);
        x_pkt.pts = tc->avOutputVideoEncoderCtx->coded_frame->pts;
        log(DEBUG, "post-encoding pts %"PRId64"\n", x_pkt.pts);
        x_pkt.pts = av_rescale_q(x_pkt.pts,
                tc->avOutputVideoEncoderCtx->time_base,
                tc->avOutputFmtCtx->streams[tc->video_ind]->time_base);
        log(DEBUG, "writing with pts %"PRId64"\n", x_pkt.pts);
        x_pkt.dts = x_pkt.pts;
        x_pkt.data = tc->encbuf;
        x_pkt.size = encbuf_size;
        x_pkt.stream_index = tc->video_ind;
        if(tc->avOutputVideoEncoderCtx->coded_frame->key_frame)
            x_pkt.flags |= AV_PKT_FLAG_KEY;

        r = tc_packet_write_and_free(tc, &x_pkt);
        if (r)
            return r;
    }
    av_free(tc->frame);
    av_free_packet(&tc->read_pkt);
    return 0;
}

static int tc_process_frame(Transcoder *tc) {
    int r;

    r = tc_read_frame(tc);
    if (r > 0) return 0;
    if (r < 0) return r;

    if (tc->read_pkt.stream_index == tc->video_ind) {
        // start decoding from the beginning,
        // to avoid having errors 'no reference frame'
        tc_decode_frame(tc);
    }

    double timestamp = tc->read_pkt.dts * av_q2d(tc->avInputFmtCtx->streams[tc->read_pkt.stream_index]->time_base);
    log(DEBUG, "timestamp %f\n", timestamp);

    if (tc->read_pkt.stream_index != tc->video_ind
            || timestamp < tc->args.encode_start_arg
            || timestamp > tc->args.encode_end_arg   ) {
        r = tc_straight_write(tc);
        if (tc->read_pkt.stream_index == tc->video_ind)
            av_free(tc->frame);
        if (r > 0) return 0;
        if (r < 0) return r;
        return 0;
    }

    r = tc_encode_write_frame(tc);
    if (r > 0) return 0;
    if (r < 0) return r;

    return 0;
}

void tc_destroy(Transcoder *tc) {
    int i;
    av_write_trailer(tc->avOutputFmtCtx); // finalize output file
    for (i = 0; i < tc->avOutputFmtCtx->nb_streams; i++) {
        avcodec_close(tc->avOutputFmtCtx->streams[i]->codec);
    }
    avio_close(tc->avOutputFmtCtx->pb);
    avformat_free_context(tc->avOutputFmtCtx);

    avcodec_close(tc->avInputVideoDecoderCtx);
    avformat_close_input(&tc->avInputFmtCtx);

    cmdline_parser_free(&tc->args);
}

int tc_input_open(Transcoder *tc) {
    int i;
    int r;
    r = avformat_open_input(&tc->avInputFmtCtx, tc->args.input_arg, NULL, NULL);
    if (r) {
        log(ERROR, "open input fail\n");
        return 1;
    }

    r = avformat_find_stream_info(tc->avInputFmtCtx, NULL);
    if (r < 0) {
        log(ERROR, "probing input fail\n");
        return 1;
    }
    av_dump_format(tc->avInputFmtCtx, 0, tc->args.input_arg, 0);

    for (i = 0; i < tc->avInputFmtCtx->nb_streams; i++) {
        AVStream *inputStream = tc->avInputFmtCtx->streams[i];
        if (inputStream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            tc->video_ind = i;
        }
    }
    assert(tc->video_ind != -1);
    tc->avInputVideoDecoderCtx = tc->avInputFmtCtx->streams[tc->video_ind]->codec;

    return 0;
}

int tc_output_open(Transcoder *tc) {
    int r;
    int i;
    tc->avOutputFmtCtx = avformat_alloc_context();
    if (!tc->avOutputFmtCtx) {
        log(ERROR, "avformat_alloc_context fail\n");
        return 1;
    }

    tc->avOutputFmtCtx->oformat = av_guess_format(NULL, tc->args.output_arg, NULL);
    if (!tc->avOutputFmtCtx->oformat) {
        log(ERROR, "choose output format fail\n");
        return 1;
    }

    r = avio_open(&tc->avOutputFmtCtx->pb, tc->args.output_arg, AVIO_FLAG_WRITE);
    if (r) {
        log(ERROR, "output file %s open fail\n", tc->args.output_arg);
        return 1;
    }

    for (i = 0; i < tc->avInputFmtCtx->nb_streams; i++) {
        AVStream *inputStream = tc->avInputFmtCtx->streams[i];
        AVStream *newStream = avformat_new_stream(tc->avOutputFmtCtx, inputStream->codec->codec);
        if (!newStream) {
            log(ERROR, "avformat_new_stream fail\n");
            return 1;
        }

        if(tc->avOutputFmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
            newStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

        if (inputStream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVCodec *outputEncoder = avcodec_find_encoder(inputStream->codec->codec_id);
            r = avcodec_get_context_defaults3(newStream->codec, outputEncoder);
            if (r) {
                log(ERROR, "avcodec_get_context_defaults3 fail\n");
                return 1;
            }
            tc->video_ind = i;
            tc->avOutputVideoEncoderCtx = newStream->codec;

#if 0
            if (inputStream->codec->extradata_size) {
                log(DEBUG, "copying %d bytes extradata for stream %d\n",
                        inputStream->codec->extradata_size, i);
                newStream->codec->extradata = malloc(inputStream->codec->extradata_size);
                assert(newStream->codec->extradata);
                memcpy(newStream->codec->extradata, inputStream->codec->extradata,
                        inputStream->codec->extradata_size);
                newStream->codec->extradata_size = inputStream->codec->extradata_size;
                dump_buf(newStream->codec->extradata, newStream->codec->extradata_size);
            }
#endif

            newStream->codec->pix_fmt = PIX_FMT_YUV420P;
            newStream->codec->width = inputStream->codec->width;
            newStream->codec->height = inputStream->codec->height;

            newStream->codec->time_base = inputStream->codec->time_base;
            log(DEBUG, "video decoder time_base %d/%d\n",
                    inputStream->codec->time_base.num,
                    inputStream->codec->time_base.den);
            log(DEBUG, "video encoder time_base %d/%d\n",
                    newStream->codec->time_base.num,
                    newStream->codec->time_base.den);

            newStream->sample_aspect_ratio = (AVRational){1, 1};
            newStream->codec->sample_aspect_ratio = (AVRational){1, 1};
            newStream->codec->bit_rate =
                tc->args.v_bitrate_arg ? : inputStream->codec->bit_rate;
            newStream->codec->rc_max_rate = newStream->codec->bit_rate;
            newStream->codec->bit_rate_tolerance = newStream->codec->bit_rate / 10;
            newStream->codec->rc_buffer_size = newStream->codec->bit_rate * 1/*second*/;

            AVDictionary *v_opts = NULL;
            av_dict_set(&v_opts, "profile", "main", 0);
            av_dict_set(&v_opts, "tune", "zerolatency", 0);
            av_opt_set_dict(newStream->codec->priv_data, &v_opts);
        } else {
            r = avcodec_copy_context(newStream->codec, inputStream->codec);
            if (r) {
                log(ERROR, "copying AVCodecContext fail\n");
                return 1;
            }
        }
        newStream->codec->codec_id = inputStream->codec->codec_id;
        if (newStream->codec->codec_type == AVMEDIA_TYPE_DATA)
            newStream->codec->codec_type = AVMEDIA_TYPE_SUBTITLE;

    }
    return 0;
}

int tc_decoder_open(Transcoder *tc) {
    int r;
    AVCodec *inputDecoder = avcodec_find_decoder(tc->avInputVideoDecoderCtx->codec_id);
    if (!inputDecoder) {
        log(ERROR, "decoder for codec %d not found\n", tc->avInputVideoDecoderCtx->codec_id);
        return 1;
    }
    r = avcodec_open2(tc->avInputVideoDecoderCtx, inputDecoder, NULL);
    if (r) {
        log(ERROR, "decoder open fail\n");
        return 1;
    }
    return 0;
}

int tc_encoder_open(Transcoder *tc) {
    int r;
    AVCodec *outputEncoder = avcodec_find_encoder(tc->avOutputVideoEncoderCtx->codec_id);
    if (!outputEncoder) {
        log(ERROR, "encoder for codec %d not found\n", tc->avOutputVideoEncoderCtx->codec_id);
        return 1;
    }
    r = avcodec_open2(tc->avOutputVideoEncoderCtx, outputEncoder, NULL);
    if (r) {
        log(ERROR, "encoder open fail\n");
        return 1;
    }
    return 0;
}

