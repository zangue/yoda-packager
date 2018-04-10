#ifndef YP_COMMON_H_
#define YP_COMMON_H_

#include <libavformat/avformat.h>

typedef struct YPInputStream {
    const char *filename;
    AVFormatContext *ctx;
    int stream_idx;
    int is_video;
    int is_audio;
    unsigned int set_id; // Adaptation set
    unsigned int stream_id; // Representation
    unsigned int period_id; // Period
} YPInputStream;

typedef struct YPOutputStream {
    AVFormatContext *ctx;
} YPOutputStream;

typedef struct YPConfig {
    YPInputStream **instreams;
    char *outdir;
    char *index_fname;
    char *profile;
    int min_buffer;
    int seg_duration;
    int verbose;
    int has_video;
    int has_audio;
    int single_file;
    int segment_template;
    int segment_timeline;
} YPConfig;

typedef struct YPIndexHandlerClass {
    void *opaque;
    int (*init)(struct YPIndexHandlerClass *self, YPConfig *config);
    int (*add_segment)(struct YPIndexHandlerClass 
                      *self, YPInputStream *instream,
                      char *filename,
                      int64_t pos,
                      int64_t size,
                      double duration,
                      int num);
    int (*finalize)(struct YPIndexHandlerClass *self);
} YPIndexHandlerClass;

typedef struct YPMuxerClass {
    void *opaque; // Muxer private data
    struct YPIndexHandlerClass *index;
    int (*init)(struct YPMuxerClass *self, YPConfig *config, int instream_index);
    int (*handle_packet)(struct YPMuxerClass *self, 
                         struct YPInputStream* instream,
                         AVPacket *pkt);
    int (*finalize)(struct YPMuxerClass *self);
} YPMuxerClass;
#endif // YP_COMMON_H_
