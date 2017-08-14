#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

#define IO_BUFFER_SIZE    32768

typedef struct OutputStream {
    AVFormatContext *ctx;
    // I/O buffer use to store data in process
    // Will be attached to the output format context and filled by the mp4 muxer
    uint8_t iobuf[IO_BUFFER_SIZE];
    // I/O context used to write files
    AVIOContext *out;
    int64_t start_pts;
    int64_t end_pts;
    double duration;
    int init_end;
    int segment_written;
    int seg_num;
    const char *seg_name_pattern;
} OutputStream;


static void write_styp(AVIOContext *pb);

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           tag,
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static int flush_buffer(OutputStream *os)
{
    int ret = 0;
    char filename[1024];

    snprintf(filename, sizeof(filename), os->seg_name_pattern, os->seg_num);
        
    // Open segment file for writing
    ret = os->ctx->io_open(os->ctx, &os->out, filename, AVIO_FLAG_WRITE, NULL);

    if (ret < 0) {
        return ret;
    }

    // write_styp(os->ctx->pb);
    // flush muxer buffer
    av_write_frame(os->ctx, NULL);
    avio_flush(os->ctx->pb);

    os->segment_written = 0;

    // close file
    os->ctx->io_close(os->ctx, os->out);

    os->seg_num++;

    return ret;
}

static int handle_packet(AVFormatContext *ifmt_ctx, OutputStream *os, AVPacket *pkt)
{
    int ret = 0;
    char seg_filename[1024];
    int64_t segment_duration = 10000000; // 10 sec

    log_packet(ifmt_ctx, pkt, "in");

    if (os->start_pts == AV_NOPTS_VALUE) {
        os->start_pts = pkt->pts;
        os->end_pts = pkt->pts;
    }

    // We still need to write initialization file for this stream
    // We use av_write_frame() call on our mp4 muxer for this purpose
    if (!os->init_end) {
        // Passing NULL here will cause the muxer to immediatly flush data
        // buffered within it
        av_write_frame(os->ctx, NULL);
        // Now set the byte range boundary of the init file
        os->init_end = avio_tell(os->ctx->pb);
        // Close init file handler
        os->ctx->io_close(os->ctx, os->out);
        //ifmt_ctx->io_close(ifmt_ctx, os->out);
    }

    // Generate segments here:
    // Packets containing key frame will have the AV_PKT_FLAG_KEY flag set:
    // pkt->flags & AV_PKT_FLAG_KEY.
    // Flush segment if we hit a key frame and the desired segment duration
    if (pkt->flags & AV_PKT_FLAG_KEY && os->segment_written &&
            av_compare_ts(pkt->pts - os->start_pts, ifmt_ctx->streams[0]->time_base, segment_duration, AV_TIME_BASE_Q)) {
        printf("Key frame hit!\n");

        ret = flush_buffer(os);

        if (ret < 0) return ret;
    }

    os->segment_written = 1;
    // Write packets to mp4 muxer
    // TODO: check ff_write_chained() method impl. for best practice
    ret = av_write_frame(os->ctx, pkt);

    return ret;
}

static int write_buffer(void *opaque, uint8_t *buf, int buf_size)
{
    OutputStream *os = opaque;
    if (os->out)
        avio_write(os->out, buf, buf_size);
    return buf_size;
}

static void write_styp(AVIOContext *pb)
{
    avio_wb32(pb, 24);
    avio_wl32(pb, MKTAG('s', 't', 'y', 'p'));
    avio_wl32(pb, MKTAG('m', 's', 'd', 'h'));
    avio_wb32(pb, 0); /* minor */
    avio_wl32(pb, MKTAG('m', 's', 'd', 'h'));
    avio_wl32(pb, MKTAG('m', 's', 'i', 'x'));
}

int main(int argc, char **argv)
{
    const char *filename;
    AVOutputFormat *oformat; // Output container format
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket pkt;
    char *in_filename, *out_filename;
    int ret;
    AVDictionary *opt = NULL;
    int i, video_stream_idx;
    char init_filename[1024];

    OutputStream *os = malloc(sizeof(OutputStream));

    os->seg_num = 0;
    os->seg_name_pattern = "seg-%d.m4s";

    /* Initialize libavcodec, and register all codecs and formats. */
    av_register_all();

    if (argc < 2) {
        printf("usage: %s input_file\n"
               "API example program to output a media file with libavformat.\n"
               "This program generates a synthetic audio and video stream, encodes and\n"
               "muxes them into a file named output_file.\n"
               "The output format is automatically guessed according to the file extension.\n"
               "Raw images can also be output by using '%%d' in the filename.\n"
               "\n", argv[0]);
        return 1;
    }

    in_filename = argv[1];

    /*
     * avformat_open_input() will do the avformat_alloc_context() for us!
     */
    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        fprintf(stderr, "Could not open input file '%s'\n", filename);
        return -1;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        return -1;
    }

    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    /* Look for the first video stream */
    video_stream_idx = -1;
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            printf("Found video stream at index %d\n", i);
            break;
        }
    }

    if (video_stream_idx == -1) {
        fprintf(stderr, "Couldnt find video stream\n");
        return -1;
    }


    /* Look for mp4 muxer
     * From doc:
     * AVFormatContext.oformat field must be set to select the muxer that will
     * be used
     */
    oformat = av_guess_format("mp4", NULL, NULL);

    if (!oformat) {
        fprintf(stderr, "Could not find an appropriate muxer\n");
        return AVERROR_MUXER_NOT_FOUND;
    }

    // Prepare output: create context and stream
    // AVFormatContext *octx;
    AVStream *st;
    AVDictionary *opts;

    ofmt_ctx = avformat_alloc_context();

    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        return -1;
    }

    // Reference output context in output stream
    os->ctx = ofmt_ctx;

    // Populate context with relevant informations:
    // Set this field to select the muxer that will be used, i.e. the mp4 muxer
    ofmt_ctx->oformat = oformat;
    ofmt_ctx->avoid_negative_ts = ifmt_ctx->avoid_negative_ts;
    ofmt_ctx->flags = ifmt_ctx->flags;
    // Allocate and initialize AVIOContext (for buffered I/O) of our output stream
    ofmt_ctx->pb = avio_alloc_context(os->iobuf, sizeof(os->iobuf), AVIO_FLAG_WRITE, os, NULL, write_buffer, /* seek */ NULL);
    ofmt_ctx->interrupt_callback = ifmt_ctx->interrupt_callback;
    // User private data
    ofmt_ctx->opaque = os;
    ofmt_ctx->io_close = ifmt_ctx->io_close;
    ofmt_ctx->io_open = ifmt_ctx->io_open;
    //ofmt_ctx->avoid_negative_ts = ifmt_ctx->avoid_negative_ts;
    //ofmt_ctx->flags = ifmt_ctx->flags;

    // The output stream
    st = avformat_new_stream(ofmt_ctx, NULL);

    if (!st) {
        fprintf(stderr, "Could not create new stream\n");
        return -1;
    }
    // Copy input stream parameters
    avcodec_parameters_copy(st->codecpar, ifmt_ctx->streams[video_stream_idx]->codecpar);
#if 0    
    if (av_codec_get_id(ofmt_ctx->oformat->codec_tag, ifmt_ctx->streams[video_stream_idx]->codecpar->codec_tag) == st->codecpar->codec_id) {
        printf("YEYEYEYEY\n");
    }

    if (av_codec_get_tag(ofmt_ctx->oformat->codec_tag, ifmt_ctx->streams[video_stream_idx]->codecpar->codec_id) <= 0) {
        printf("YEKE YEKE\n");
    }

    if (!ofmt_ctx->oformat->codec_tag) {
        printf("MORY KANTE\n");
    }
#endif
    // NOTE:
    // We are safe when we set this field to 0. I ran into an issue when
    // copying this value from input format context. Actually, according
    // to docs it's recommended to set relevant codecpar fields individually
    // rather than copying it from input format context.
    st->codecpar->codec_tag = 0;

    st->sample_aspect_ratio = ifmt_ctx->streams[video_stream_idx]->sample_aspect_ratio;
    st->time_base = ifmt_ctx->streams[video_stream_idx]->time_base;
    av_dict_copy(&st->metadata, ifmt_ctx->streams[video_stream_idx]->metadata, 0);

    //const AVCodecDescriptor *cd;
    //if ((cd = avcodec_descriptor_get(st->codecpar->codec_id))) {
    //    printf("Codec name = %s\n", cd->name);
    //}

    snprintf(init_filename, sizeof(init_filename), "%s", "init.mp4");
    ret = ifmt_ctx->io_open(ifmt_ctx, &os->out, init_filename, AVIO_FLAG_WRITE, NULL);
    // ret = ofmt_ctx->io_open(ofmt_ctx, &os->out, init_filename, AVIO_FLAG_WRITE, NULL);

    if (ret < 0) {
        return ret;
    }

    // Set option for the mp4 muxer
    av_dict_set(&opts, "movflags", "frag_custom+dash+delay_moov", 0);

    // Allocate the output stream private data and initialize the codec, but do
    // not write the header. May optionally be used before
    // avformat_write_header() to initialize stream parameters before actually
    // writing the header.
    // If using this function do not pass the same option to
    // avformat_write_header()
    if ((ret = avformat_init_output(ofmt_ctx, &opts)) < 0)
        return ret;

    if ((ret = avformat_write_header(ofmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        //goto end;
        return ret;
    }

    // Free resources
    avio_flush(ofmt_ctx->pb);
    //av_free(ofmt_ctx->pb);
    av_dict_free(&opts);

    os->start_pts = AV_NOPTS_VALUE;
    os->end_pts = AV_NOPTS_VALUE;
    os->duration = 0;

    while (1) {
        ret = av_read_frame(ifmt_ctx, &pkt);

        if (ret < 0) {
            printf("No frame left\n");
            break;
        }

        if (pkt.stream_index == video_stream_idx) {
            ret = handle_packet(ifmt_ctx, os, &pkt);
        }

        if (ret < 0) break;

        av_packet_unref(&pkt);
    }

    /* free the stream */
    //avformat_free_context(ifmt_ctx);
    avformat_free_context(ofmt_ctx);
    avformat_close_input(&ifmt_ctx);
    free(os);

    return 0;
}

