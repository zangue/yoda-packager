#include "common.h"
#include "third_party/argtable3.h"
#include "muxer.h"
#include "mpd.h"
#include "utils.h"

#include <libavutil/dict.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/intreadwrite.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

static int open_input_file(YPInputStream *instream)
{
    int ret;
    unsigned int i;
    AVFormatContext *ifmt_ctx = NULL;

    ret = 0;
    printf("Opening input file");
    printf("%s\n", instream->filename);

    /*
     * avformat_open_input() will do the avformat_alloc_context() for us!
     */
    if ((ret = avformat_open_input(&ifmt_ctx, instream->filename, 0, 0)) < 0) {
        fprintf(stderr, "could not open input file '%s'\n", instream->filename);
        // TODO: free ressources
        return -1;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        fprintf(stderr, "failed to retrieve input stream information");
        // TODO: free ressources
        return -1;
    }

    av_dump_format(ifmt_ctx, 0, instream->filename, 0);
    
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(ifmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
        printf("%s=%s\n", tag->key, tag->value);

    char tag_str[64];

    AV_WL32(&tag_str, ifmt_ctx->streams[1]->codecpar->codec_tag);
    tag_str[4] = '\0';

    printf ("Profile: %d\n", ifmt_ctx->streams[0]->codecpar->profile);
    printf ("Level: %d\n", ifmt_ctx->streams[0]->codecpar->level);
    printf ("Codec tag: %d\n", ifmt_ctx->streams[0]->codecpar->codec_tag);
    printf ("Codec tag string: %s\n", tag_str);
    
    // MPEG-4 Part 15 "Advanced Video Coding (AVC) file format
    // 5.2.4.1.1 Syntax 
    //
    // aligned(8) class AVCDecoderConfigurationRecord { 
    //     unsigned int(8) configurationVersion = 1; 
    //     unsigned int(8) AVCProfileIndication; 
    //     unsigned int(8) profile_compatibility; 
    //     unsigned int(8) AVCLevelIndication;  
    //     bit(6) reserved = ‘111111’b;
    //     unsigned int(2) lengthSizeMinusOne;  
    //     bit(3) reserved = ‘111’b;
    //     unsigned int(5) numOfSequenceParameterSets; 
    //     for (i=0; i< numOfSequenceParameterSets;  i++) { 
    //         unsigned int(16) sequenceParameterSetLength ; 
    //     bit(8*sequenceParameterSetLength) sequenceParameterSetNALUnit; 
    // } 
    //  unsigned int(8) numOfPictureParameterSets; 
    //  for (i=0; i< numOfPictureParameterSets;  i++) { 
    //       unsigned int(16) pictureParameterSetLength; 
    //  bit(8*pictureParameterSetLength) pictureParameterSetNALUnit; 
    //  } 
    // }
    printf ("%02x%02x%02x\n\n",
            ifmt_ctx->streams[0]->codecpar->extradata[1],
            ifmt_ctx->streams[0]->codecpar->extradata[2],
            ifmt_ctx->streams[0]->codecpar->extradata[3]);

    // TODO: assert nb_streams > 0

    instream->ctx = ifmt_ctx;

    return ret;
}


// TODO
// - Rename this method to sort stream
// - sort stream into adaptation set
// - video adaptation set = content type + codec id
// - audio adaptation set = content type + codec id + lang
static int tag_streams(YPInputStream** instreams, int n)
{
    int i = 0;
    unsigned int period_id = 0;

    for (i = 0; i < n; i++) {
        AVStream *st = instreams[i]->ctx->streams[instreams[i]->stream_idx];
        
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            instreams[i]->period_id = period_id;
            instreams[i]->is_video = 1;
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            instreams[i]->period_id = period_id;
            instreams[i]->is_audio = 1;
        }

    }

    return 0;
}

static int feed_muxer(YPMuxerClass *muxer, YPInputStream *instream)
{
    int ret = 0;
    AVPacket pkt;

    while (1) {
        ret = av_read_frame(instream->ctx, &pkt);

        if (ret < 0) {
            printf("No frame left\n");
            break;
        }

        // TODO:
        // Rescale timing infos here (timescales, and durations)

        if (pkt.stream_index == instream->stream_idx) {
            ret = muxer->handle_packet(muxer, instream, &pkt);
        }

        if (ret < 0) break;

        av_packet_unref(&pkt);
    }

    muxer->finalize(muxer);

    return ret;
}

int main(int argc, char **argv)
{
    int ret, i;
    YPConfig config;
    YPIndexHandlerClass *manifest = NULL;
    YPMuxerClass **muxers = NULL;

    config.instreams = NULL;

    const char *prog_name = "ypackager";
    struct arg_file *infiles = arg_filen("i", NULL, NULL, 1, argc+2, "input file(s)");
    //struct arg_lit *outdir = arg_lit1("o", "out", "output directory");
    struct arg_int *segment_duration = arg_int1(NULL, "segment-duration", NULL, "max. segment duration in milliseconds"); 
    struct arg_lit *help = arg_lit0("h", "help", "print help and exit");
    struct arg_lit *version = arg_lit0(NULL, "version", "print version and exit");
    struct arg_lit *single_file = arg_lit0(NULL, "single-file", "write segments into single file.");
    struct arg_lit *segment_template = arg_lit0(NULL, "segment-template", "use segment template");
    struct arg_lit *segment_timeline = arg_lit0(NULL, "segment-timeline", "use segment timeline");
    struct arg_end *end = arg_end(20);

    void *argtable[] = {
        infiles,
        //outdir,
        segment_duration,
        single_file,
        segment_template,
        segment_timeline,
        help,
        version,
        end
    };

    int nerrors;
    int exit_code = 0;
    
    nerrors = arg_parse(argc, argv, argtable);

    if (arg_nullcheck(argtable) != 0) {
        printf("%s: insufficient memory\n", prog_name);
        exit_code = -1;
        goto exit;
    }

    if (help->count > 0) {
        printf("Usage: %s", prog_name);
        arg_print_syntax(stdout, argtable, "\n");
        arg_print_glossary(stdout, argtable, "  %-25s %s\n");
        exit_code = 0;
        goto exit;
    }

    if (version->count > 0) {
        printf("%s version 0.1 Copyright (c) 2017 Armand Zangue\n", prog_name);
        exit_code = 0;
        goto exit;
    }

    /* If the parser returned any errors then display them and exit */
    if (nerrors > 0) {
        /* Display the error details contained in the arg_end struct.*/
        arg_print_errors(stdout, end, prog_name);
        printf("Try '%s --help' for more information.\n", prog_name);
        exit_code = -1;
        goto exit;
    }

    /* Initialize libavcodec, and register all codecs and formats. */
    av_register_all();
    
    // Configure --------------------
    config.single_file = single_file->count;
    config.segment_template = segment_template->count;
    config.segment_timeline = segment_timeline->count;
    config.seg_duration = segment_duration->ival[0];
    config.min_buffer = config.seg_duration * 2;
    config.verbose = 1;
    config.index_fname = "init.mp4";
    config.has_video = 0;
    config.has_audio = 0;
    //config.has_subtitle = 0;

    printf("Create instreams and muxer\n"); 
    config.instreams = (YPInputStream **) malloc(infiles->count * sizeof(YPInputStream*));
    muxers = (YPMuxerClass **) malloc(infiles->count * sizeof(YPMuxerClass*));
    manifest = yp_mpd_generator();

    if (manifest == NULL) {
        exit_code = -1;
        goto exit;
    }

    for (i = 0; i < infiles->count; i++) {
        printf("filname: %s - duration: %d\n", infiles->filename[i], *segment_duration->ival);
        config.instreams[i] = (YPInputStream *) malloc(sizeof(YPInputStream));
        config.instreams[i]->filename = *infiles->filename;
        config.instreams[i]->stream_idx = 0; // TODO: write utility function to parse stream idx
        config.instreams[i]->is_video = 0;
        config.instreams[i]->is_audio = 0;
        printf("Opening instream\n");
        // TODO: Handle could not open file
        open_input_file(config.instreams[i]);
        
        AVStream *st = config.instreams[i]->ctx->streams[config.instreams[i]->stream_idx];

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            config.has_video = 1;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            config.has_audio = 1;

        printf("Creating muxer for stream\n");
        muxers[i] = yp_fmp4_muxer();
        muxers[i]->index = manifest;
    }

    // TODO: assert has_video + has_audio > 0

    // TODO:
    // This method should sort stream by periods and
    // type.
    // Maybe return an array of period?
    tag_streams(config.instreams, infiles->count); 
    // Configure end -----------------

    // Init index handle --------------
    manifest->init(manifest, &config);
    // End ----------------------------

    // Init muxers --------------------
    for (i = 0; i < infiles->count; i++) {
        // TODO: handle muxer erros
        printf("Init muxer for instream %d\n", i);
        ret = muxers[i]->init(muxers[i], &config, i);
    }
    // Init muxers end ----------------


    // feed muxers --------------------
    for (i = 0; i < infiles->count; i++) {
        // TODO: handle muxer erros
        printf("Feed data into muxer for instream %d\n", i);
        ret = feed_muxer(muxers[i], config.instreams[i]);
    }
    // feed muxers end ----------------

    manifest->finalize(manifest);

exit:
    if (manifest != NULL)
        yp_mpd_generator_free(manifest);

    if (muxers != NULL) {
        for (i = 0; i < infiles->count; i++) {
            if (muxers[i])
                yp_muxer_free(muxers[i]);      
        }
        free(muxers);
    }

    if (config.instreams != NULL) {
        printf("freeing count: %d\n", infiles->count);
        for (i = 0; i < infiles->count; i++) {
            if (config.instreams[i]){
                printf("free single %p\n", config.instreams[i]);
                free(config.instreams[i]);
            }
        }
        free(config.instreams);
    }
    arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return exit_code;


    //return 0;
}
