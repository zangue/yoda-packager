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

#include "common.h"

#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC

#define IO_BUFFER_SIZE    32768

typedef struct OutputStream {
    YPInputStream *instream;
    AVFormatContext *avfctx;
    // I/O buffer use to store data in process
    // Will be attached to the output format context and filled by the mp4 muxer
    uint8_t iobuf[IO_BUFFER_SIZE];
    // I/O context used to write files
    AVIOContext *out;
    int64_t first_pts;
    int64_t curr_pts;
    int64_t last_pts;
    int64_t segment_duration;
    double last_segment_duration;
    double duration;
    int init_segment_end;
    int segment_written;
    int segment_num;
    const char *segment_name_pattern;
    int single_file;
    // bandwidth
    // is_video
    // is_audio
    // outdir: argx/<type>_<bitrate>\
    // profile: live vs vod
} OutputStream;

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

static int write_buffer(void *opaque, uint8_t *buf, int buf_size)
{
    OutputStream *os = opaque;
    if (os->out)
        avio_write(os->out, buf, buf_size);
    return buf_size;
}

static int flush_buffer(YPMuxerClass *self, OutputStream *os)
{
    int ret = 0;
    char filename[1024];
    int64_t pos;
    int64_t size;
    int duration;
    AVStream *st;

    snprintf(filename, sizeof(filename), os->segment_name_pattern, os->segment_num);
        
    // Open segment file for writing
    ret = os->avfctx->io_open(os->avfctx, &os->out, filename, AVIO_FLAG_WRITE, NULL);

    if (ret < 0) {
        return ret;
    }

    // Get segment start position in bytes
    pos = avio_tell(os->avfctx->pb);

    // flush muxer buffer
    av_write_frame(os->avfctx, NULL);
    avio_flush(os->avfctx->pb);

    os->segment_written = 0;

    // close file
    os->avfctx->io_close(os->avfctx, os->out);

    size = avio_tell(os->avfctx->pb) - pos;
    st = os->instream->ctx->streams[os->instream->stream_idx];

    os->segment_num++;

    // printf("Segment written, segment duration: %f\n", os->last_segment_duration);

    // Add segment to index
    self->index->add_segment(
            self->index,
            os->instream,
            filename,
            pos, size,
            os->last_segment_duration,
            os->segment_num);
    return ret;
}

static int fmp4_init(YPMuxerClass *self, YPConfig *config, int instream_index)
{
    AVFormatContext *ofmt_ctx = NULL;
    AVFormatContext *ifmt_ctx = NULL;
    AVOutputFormat *oformat = NULL;
    AVStream *st = NULL; // stream for output
    AVDictionary *opts = NULL;
    char init_filename[1024];
    int ret = 0;
    OutputStream *os = malloc(sizeof(OutputStream));

    if (os == NULL) {
        return -1;
    }

    //os->segment_base_dir = ""
    os->segment_num = 0;
    os->segment_duration = config->seg_duration * 1000; // microseconds
    //printf("SEG DURAION: %" PRId64 " \n\n\n", os->segment_duration);
    os->segment_name_pattern = "seg-%d.m4s";
    os->segment_written = 0;
    os->init_segment_end = 0;
    os->first_pts = AV_NOPTS_VALUE;
    os->curr_pts = AV_NOPTS_VALUE;
    os->last_pts = AV_NOPTS_VALUE;
    os->duration = 0;
    os->instream = config->instreams[instream_index];
    os->single_file = config->single_file;

    ifmt_ctx = os->instream->ctx;

    /* Look for mp4 muxer
     * From doc:
     * AVFormatContext.oformat field must be set to select the muxer that will
     * be used
     */
    oformat = av_guess_format("mp4", NULL, NULL);

    if (!oformat) {
        fprintf(stderr, "Could not find an appropriate muxer\n");
        // TODO: free ressources
        return AVERROR_MUXER_NOT_FOUND;
    }

    ofmt_ctx = avformat_alloc_context();

    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        // TODO: free ressources
        return -1;
    }

    // Reference output context in output stream
    os->avfctx = ofmt_ctx;

    // Populate context with relevant informations:
    // Set this field to select the muxer that will be used, i.e. the mp4 muxer
    ofmt_ctx->oformat = oformat;
    ofmt_ctx->avoid_negative_ts = os->instream->ctx->avoid_negative_ts;
    ofmt_ctx->flags = os->instream->ctx->flags;
    // Allocate and initialize AVIOContext (for buffered I/O) of our output stream
    ofmt_ctx->pb = avio_alloc_context(os->iobuf, sizeof(os->iobuf), AVIO_FLAG_WRITE, os, NULL, write_buffer, /* seek */ NULL);
    ofmt_ctx->interrupt_callback = os->instream->ctx->interrupt_callback;
    // User private data
    ofmt_ctx->opaque = os;
    ofmt_ctx->io_close = os->instream->ctx->io_close;
    ofmt_ctx->io_open = os->instream->ctx->io_open;
    ofmt_ctx->avoid_negative_ts = os->instream->ctx->avoid_negative_ts;

    // The output stream
    st = avformat_new_stream(ofmt_ctx, NULL);

    if (!st) {
        fprintf(stderr, "Could not create new stream\n");
        // TODO: free resources
        return -1;
    }

    // Copy input stream parameters
    avcodec_parameters_copy(st->codecpar, os->instream->ctx->streams[os->instream->stream_idx]->codecpar);

    // NOTE:
    // We are safe when we set this field to 0. I ran into an issue when
    // copying this value from input format context. Actually, according
    // to docs it's recommended to set relevant codecpar fields individually
    // rather than copying it from input format context.
    st->codecpar->codec_tag = 0;

    st->sample_aspect_ratio = os->instream->ctx->streams[os->instream->stream_idx]->sample_aspect_ratio;
    st->time_base = os->instream->ctx->streams[os->instream->stream_idx]->time_base;
    av_dict_copy(&st->metadata, os->instream->ctx->streams[os->instream->stream_idx]->metadata, 0);

    //const AVCodecDescriptor *cd;
    //if ((cd = avcodec_descriptor_get(st->codecpar->codec_id))) {
    //    printf("Codec name = %s\n", cd->name);
    //}
    
    snprintf(init_filename, sizeof(init_filename), "%s", "init.mp4");
    ret = ofmt_ctx->io_open(ofmt_ctx, &os->out, init_filename, AVIO_FLAG_WRITE, NULL);

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
    if ((ret = avformat_init_output(ofmt_ctx, &opts)) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        return ret;
    }

    if ((ret = avformat_write_header(ofmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        //goto end;
        return ret;
    }

    avio_flush(ofmt_ctx->pb);
    av_dict_free(&opts);

    self->opaque = (void*) os;

    return 0;
}


static int is_keyframe(AVPacket *pkt)
{
    if (pkt->flags == AV_PKT_FLAG_KEY) {
        return 0;
    }

    return -1;
}

static int fmp4_handle_packet(YPMuxerClass *self, YPInputStream *instream, AVPacket *pkt)
{
    int ret = 0;
    int i;
    char seg_filename[1024];
    AVStream *st;
    OutputStream *os = self->opaque;

    //log_packet(os->instream->ctx, pkt, "in");

    if (os->first_pts == AV_NOPTS_VALUE) {
        os->first_pts = pkt->pts;
        os->last_pts = pkt->pts;
    }

    os->curr_pts = pkt->pts + pkt->duration;

    // We still need to write initialization file for this stream
    // We use av_write_frame() call on our mp4 muxer for this purpose
    if (!os->init_segment_end) {
        // Passing NULL here will cause the muxer to immediatly flush data
        // buffered within it
        av_write_frame(os->avfctx, NULL);
        // Now set the byte range boundary of the init file
        os->init_segment_end = avio_tell(os->avfctx->pb);
        // Close init file handler
        os->avfctx->io_close(os->avfctx, os->out);
        // TODO: inform indexer about init range lenght for this stream
        // bzw. representation
    }

    i = os->instream->stream_idx;
    st = os->instream->ctx->streams[i];
    
    printf("Packet: duration since last pts %" PRId64  "\n", pkt->pts - os->last_pts);
    printf("Packets Duration in seconds since last %f\n", (double)(pkt->pts - os->last_pts)*
                st->time_base.num/st->time_base.den);

    printf("Configure segment duration in microseconds %f\n", (double)os->segment_duration);
    printf("Comparison: %i\n", av_compare_ts(pkt->pts - os->last_pts, st->time_base, os->segment_duration, AV_TIME_BASE_Q));

    // Generate segments here:
    // Packets containing key frame will have the AV_PKT_FLAG_KEY flag set:
    // pkt->flags & AV_PKT_FLAG_KEY.
    // Flush segment if we hit a key frame and the desired segment duration
    // TODO:
    // if we have configured duration but no key frame set exit on error: New
    // segments must start with keyframe!
    if (/*pkt->flags & AV_PKT_FLAG_KEY && */os->segment_written &&
            av_compare_ts(pkt->pts - os->last_pts, st->time_base,
                            os->segment_duration, AV_TIME_BASE_Q) >= 0) {
        printf("Checking for key frame....\n");
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            printf("Key frame hit! %" PRId64  "\n", pkt->pts - os->last_pts);
            printf("%f\n", (double)(pkt->pts - os->last_pts)*st->time_base.num/st->time_base.den);
            printf("%f\n", (double)os->segment_duration);

            os->last_segment_duration = (double) (pkt->pts - os->last_pts)*st->time_base.num/st->time_base.den;
            printf("Last segment duration: %f\n", os->last_segment_duration);

            ret = flush_buffer(self, os);
            os->last_pts = os->curr_pts;
            if (ret < 0) return ret;

        } else {
            // TODO:
            // Look for a way to generate key frame pkt from pkt
            printf("New media segment should always start with a key frame\n");
        }
    }

    // TODO: Update curr_pts here instead?

    os->segment_written = 1;
    // Write packets to mp4 muxer
    // TODO: check ff_write_chained() method impl. for best practice
    ret = av_write_frame(os->avfctx, pkt);

    return ret;
}

static int fmp4_finalize(YPMuxerClass *self)
{
    OutputStream *os = self->opaque;

    // TODO: free all allocted resources
    //av_free(ofmt_ctx->pb);
    //avformat_free_context(ifmt_ctx);
    avformat_free_context(os->avfctx);
    //avformat_close_input(&ifmt_ctx);
    free(os);
    return 0;
}

// Constructor
YPMuxerClass* yp_fmp4_muxer(void)
{
    YPMuxerClass *muxer = (YPMuxerClass *) malloc(sizeof(YPMuxerClass));

    if (muxer) {
        muxer->init = &fmp4_init;
        muxer->handle_packet = &fmp4_handle_packet;
        muxer->finalize = &fmp4_finalize;
        muxer->opaque = NULL;

        return muxer;
    }

    return NULL;
}

// Destructor
void yp_muxer_free(YPMuxerClass *muxer)
{
    // if (muxer->opaque != NULL) free(muxer->opaque);
    free(muxer);
}
