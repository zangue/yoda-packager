#ifndef YP_MPD_H_
#define YP_MPD_H_

#include "common.h"

typedef struct YPSegment {
    char filename[1024];
    int64_t pos;
    int64_t size;
    int64_t index_size;
    double duration;
    int num;
    struct YPSegment *next;
} YPSegment;

typedef struct YPRepresentation {
    int id;
    int64_t bandwidth;
    char codecs[100];
    int height;
    int width;
    AVRational avg_frame_rate;
    unsigned int nb_segments;
    double total_duration;
    YPSegment *segments;
    YPSegment *last_segment;
} YPRepresentation;

typedef struct YPAdaptationSet {
    int id;
    const char *content_type;
    char *bit_stream_switching;
    const char *mime_type;
    unsigned int nb_reps;
    YPRepresentation **representations;
} YPAdaptationSet;

typedef struct YPPeriod {
    int id;
    int start_time;
    unsigned int nb_asets;
    YPAdaptationSet **asets;
} YPPeriod;

typedef struct YPMPD {
    int single_file;
    int segment_template;
    int segment_timeline;
    char *profiles;
    int min_buffer_time;
    int max_segment_duration;
    char *type;
    int min_update_period;
    int time_shift_buffer_depth;
    unsigned int nb_periods;
    YPPeriod **periods;
} YPMPD;

YPIndexHandlerClass* yp_mpd_generator(void);
void yp_mpd_generator_free(YPIndexHandlerClass *self);

#endif // YP_MPD_H_
