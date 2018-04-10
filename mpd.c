#define MPD_NS                    "urn:mpeg:dash:schema:mpd:2011"
#define ISOBMFF_ON_DEMAND_PROFILE "urn:mpeg:dash:profile:isoff-ondemand:2011"
#define ISOBMFF_LIVE_PROFILE      "urn:mpeg:dash:profile:isoff-live:2011"

#include <stdlib.h>
#include <math.h>
#include <libavutil/avstring.h>
#include <libavutil/intreadwrite.h>

#include "mpd.h"

static void mpd_output_representation(AVIOContext *out, YPRepresentation *representation);
static void mpd_output_segment_template(AVIOContext *out, int duration, int timescale);


static void set_rfc6381_codec_name(AVCodecParameters *codec_par, char *buf, int size)
{
    AV_WL32(buf, codec_par->codec_tag);
    buf[4] = '\0';
    av_strlcatf(buf, size, ".%02x%02x%02x",
            codec_par->extradata[1],  // profile_idc
            codec_par->extradata[2],  // profile compatibility
            codec_par->extradata[3]); // level_idc
}

static int sec_to_iso_duration(AVIOContext *out, double seconds)
{
    double seconds_in_min = 60;
    double seconds_in_hour = seconds_in_min * 60;
    double seconds_in_day = seconds_in_hour * 24;
    double seconds_in_month = seconds_in_day * 30;
    double seconds_in_year = seconds_in_month * 365;
    double years, months, days, hours, minutes = 0;
    
    if (seconds <= 0) {
        // TODO
        return -1;
    }

    avio_printf(out, "P");

    years = floor(seconds / seconds_in_year);

    if (years > 0) {
        avio_printf(out, "%.fY", years);
        seconds -= years * seconds_in_year;
    }

    months = floor(seconds / seconds_in_month);

    if (seconds > 0 && months > 0) {
        avio_printf(out, "%.fM", months);
        seconds -= months * seconds_in_month;
    }

    days = floor(seconds / seconds_in_day);

    if (seconds > 0 && days > 0) {
        avio_printf(out, "%.fD", days);
        seconds -= days * seconds_in_day;
    }

    avio_printf(out, "T");

    hours = floor(seconds / seconds_in_hour);

    if (seconds > 0 && hours > 0) {
        avio_printf(out, "%.fH", hours);
        seconds -= hours * seconds_in_hour;
    }

    minutes = floor(seconds / seconds_in_min);

    if (seconds > 0 && minutes > 0) {
        avio_printf(out, "%.fM", minutes);
        seconds -= minutes * seconds_in_min;
    }

    if (seconds > 0) {
        avio_printf(out, "%.fS", seconds);
    }

    return 0;
}

static void mpd_free_segments(YPSegment *segments)
{
    //int nb_segments = (int) sizeof(segments)/sizeof(YPSegment*);
    //int i;

    YPSegment *temp;
    YPSegment *curr = segments;

    while (curr != NULL) {
        temp = curr->next;
        free(curr);
        curr = temp;
    }

    //for (i = 0; i < nb_segments; i++) {
    //    free(segments[i]);
    //}
    //free(segments);
}

static void mpd_free_representations(YPRepresentation **reps)
{
    int nb_reps = (int) sizeof(reps)/sizeof(YPRepresentation*);
    int i;

    for (i = 0; i < nb_reps; i++) {
        if (reps[i]->segments != NULL) {
            mpd_free_segments(reps[i]->segments);
        }
        free(reps[i]);
    }
    free(reps);

}

static void mpd_free_asets(YPAdaptationSet **asets)
{
    int nb_asets = (int) sizeof(asets)/sizeof(YPAdaptationSet*);
    int i;

    for (i = 0; i < nb_asets; i++) {
        if (asets[i]->representations != NULL) {
            mpd_free_representations(asets[i]->representations);
        }
        free(asets[i]);
    }
    free(asets);
}

static void mpd_free_periods(YPPeriod **periods)
{
    int nb_periods = (int) sizeof(periods)/sizeof(YPPeriod*);
    int i;

    for (i = 0; i < nb_periods; i++) {
        if (periods[i]->asets != NULL) {
            mpd_free_asets(periods[i]->asets);
        }
        free(periods[i]);
    }
    free(periods);
}

static void mpd_free(YPMPD *mpd) {
    if (mpd->periods != NULL) {
        mpd_free_periods(mpd->periods);
    }
    free(mpd);
}

static YPRepresentation *mpd_init_representation(const YPInputStream *instream)
{
    YPRepresentation *rep = (YPRepresentation *) malloc(sizeof(YPRepresentation));
    unsigned int st_idx = instream->stream_idx;
    AVStream *st = instream->ctx->streams[st_idx];

    if (rep == NULL) {
        return rep;
    }

    rep->id = instream->stream_id;
    rep->bandwidth = st->codecpar->bit_rate;
    set_rfc6381_codec_name(st->codecpar, rep->codecs, sizeof(rep->codecs));
    rep->height = st->codecpar->height;
    rep->width = st->codecpar->width;
    rep->avg_frame_rate = st->avg_frame_rate;
    rep->nb_segments = 0;
    rep->total_duration = 0;
    rep->segments =  NULL;
    rep->last_segment = NULL;

    return rep;
}

static YPAdaptationSet *mpd_init_aset(const unsigned int id,
                                      const char *content_type,
                                      const char *mime_type,
                                      const unsigned int nb_reps)
{
    YPAdaptationSet *aset = (YPAdaptationSet *) malloc(sizeof(YPAdaptationSet));
        
    if (aset == NULL) {
        return aset;
    } 

    aset->id = id;
    aset->content_type = content_type;
    aset->bit_stream_switching = "true";
    aset->mime_type = mime_type;
    aset->representations = malloc(nb_reps * sizeof(YPRepresentation*));

    if (aset->representations == NULL) {
        return NULL;
    }

    return aset;
}

static int mpd_init(YPIndexHandlerClass *self, YPConfig *config)
{
    YPMPD *mpd;
    int ret = 0;
    unsigned int nb_periods, nb_asets, nb_reps, nb_audio_reps, nb_video_reps, i;

    mpd = malloc(sizeof(YPMPD));

    if (mpd == NULL) {
        ret = -1;
        return ret;
    }

    nb_periods = 1; // No support of multiperiod yet
    nb_asets = config->has_video + config->has_audio;
    nb_reps = (unsigned int) sizeof(config->instreams)/sizeof(YPInputStream*);
    nb_video_reps = nb_audio_reps = 0;

    for (i = 0; i < nb_reps; i++) {
        if (config->instreams[i]->is_video) {
            nb_video_reps++;
        }
        
        if (config->instreams[i]->is_audio) {
           nb_audio_reps++;
        }
    }

    // Init mpd
    mpd->single_file = config->single_file;
    mpd->segment_template = config->segment_template;
    mpd->segment_timeline = config->segment_timeline;
    mpd->profiles = "live";
    mpd->min_buffer_time = config->min_buffer;
    mpd->max_segment_duration = config->seg_duration;
    mpd->type = "static"; // No live support yet
    mpd->min_update_period = 0;
    mpd->time_shift_buffer_depth = 0;
    mpd->nb_periods = 0;
    mpd->periods = NULL;

    mpd->periods = (YPPeriod **) malloc(nb_periods * sizeof(YPPeriod*));
    
    if (mpd->periods == NULL) {
        ret = -2;
        goto fail;
    }

    mpd->periods[0] = (YPPeriod *) malloc(sizeof(YPPeriod));

    if (mpd->periods[0] == NULL) {
        ret = -3;
        goto fail;
    }

    mpd->nb_periods = nb_periods;
    mpd->periods[0]->id = 0;
    mpd->periods[0]->start_time = 0;
    mpd->periods[0]->asets = NULL;
    mpd->periods[0]->nb_asets = 0;

    mpd->periods[0]->asets = (YPAdaptationSet **) malloc(nb_asets * sizeof(YPAdaptationSet*));

    if (mpd->periods[0]->asets == NULL) {
        ret = -3;
        goto fail;
    }
    
    int aset_id = 0;
    int rep_id = 0;

    if (config->has_video) {
        YPAdaptationSet *aset = mpd_init_aset(aset_id, "video", "video/mp4", nb_video_reps);
        
        if (aset == NULL) {
            ret = -4;
            goto fail;
        }
        
        mpd->periods[0]->asets[aset_id] = aset;
        mpd->periods[0]->nb_asets++;

        for (i = 0; i < nb_reps; i++) {
            YPRepresentation *rep;
            YPInputStream *instream = config->instreams[i];

            if (instream->is_video) {
                instream->set_id = aset_id;
                instream->stream_id = rep_id++;

                rep = mpd_init_representation(instream);

                if (rep == NULL) {
                    ret = -6;
                    goto fail;
                }

                mpd->periods[0]->asets[aset_id]->representations[i] = rep;
                mpd->periods[0]->asets[aset_id]->nb_reps++;
            }
        }
        aset_id++;
    }

    if (config->has_audio) {
        YPAdaptationSet *aset = mpd_init_aset(aset_id, "audio", "audio/mp4", nb_audio_reps);
        
        if (aset == NULL) {
            ret = -4;
            goto fail;
        }
        
        mpd->periods[0]->asets[aset_id] = aset;
        mpd->periods[0]->nb_asets++;

        for (i = 0; i < nb_reps; i++) {
            YPRepresentation *rep;
            YPInputStream *instream = config->instreams[i];

            if (instream->is_audio) {
                    instream->set_id = aset_id;
                instream->stream_id = rep_id++;

                rep = mpd_init_representation(instream);

                if (rep == NULL) {
                    ret = -6;
                    goto fail;
                }
            }
            mpd->periods[0]->asets[aset_id]->representations[i] = rep;
            mpd->periods[0]->asets[aset_id]->nb_reps++;
        }
        aset_id++;
    }

    self->opaque = (void *) mpd;
    return ret;

fail:
    mpd_free(mpd); 
    return ret;
}

static int mpd_finalize(YPIndexHandlerClass *self)
{
    AVIOContext *out = NULL;
    YPMPD *mpd = (YPMPD *) self->opaque;
    char filename[1024];
    int i, j;
    int ret = 0;
    double total_duration = mpd->periods[0]->asets[0]->representations[0]->total_duration;
    YPAdaptationSet *adaptation_set = NULL;
    YPRepresentation *representation = NULL;

    snprintf(filename, sizeof(filename), "manifest.mpd");
    ret = avio_open(&out, filename, AVIO_FLAG_WRITE);

    if (ret < 0) {
        printf("Could not open manifest for writing");
        return ret;
    }

    avio_printf(out, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    avio_printf(out, "<MPD xmlns=\""MPD_NS"\" ");
    avio_printf(out, "profiles=\""ISOBMFF_LIVE_PROFILE"\" ");
    avio_printf(out, "minBufferTime=\"");
    sec_to_iso_duration(out, (double) (mpd->min_buffer_time/1000));
    avio_printf(out, "\"  ");
    avio_printf(out, "mediaPresentationDuration=\"");
    sec_to_iso_duration(out, total_duration);
    avio_printf(out, "\"  ");
    avio_printf(out, "maxSegmentDuration=\"");
    sec_to_iso_duration(out, (double) (mpd->max_segment_duration/1000));
    avio_printf(out, "\"  ");
    avio_printf(out, "tpye=\"static\">\n");

    avio_printf(out, "\t<!-- Created with Yoda Packager -->\n");

    avio_printf(out, "\t<Period id=\"%d\">\n", mpd->periods[0]->id);


    for (i = 0; i < mpd->periods[0]->nb_asets; i++) {
        adaptation_set = mpd->periods[0]->asets[i];

        avio_printf(out, "\t\t<AdaptationSet ");
        avio_printf(out, "id=\"%d\"  ", adaptation_set->id);
        avio_printf(out, "contentType=\"%s\"  ", adaptation_set->content_type);
        avio_printf(out, "mimeType=\"%s\" ", adaptation_set->mime_type);
        avio_printf(out, "segmentAlignment=\"true\" ");
        avio_printf(out, "startWithSAP=\"1\">\n");
       
        // TODO: only do this right after adaptation tag simpel
        // For timeline etc do it add representation level
        mpd_output_segment_template(out, mpd->max_segment_duration, 1000);

        for (j = 0; j < adaptation_set->nb_reps; j++) {
            representation = adaptation_set->representations[j];
            mpd_output_representation(out, representation);
        }

        avio_printf(out, "\t\t</AdaptationSet>\n");
    }

    avio_printf(out, "\t</Period>\n");
    
    avio_printf(out, "</MPD>\n");

    avio_flush(out);
    avio_close(out);

    YPSegment *segments = mpd->periods[0]->asets[0]->representations[0]->segments;
    YPSegment *curr_seg = segments;

    // while(curr_seg != NULL) {
    //     fprintf(stdout, "segment: file: %s pos: %" PRId64 " size %" PRId64 " duration %f  num %d\n",
    //         curr_seg->filename, curr_seg->pos, curr_seg->size, curr_seg->duration, curr_seg->num);
    //     curr_seg = curr_seg->next;
    // }

    // printf("Total duration: %f\n", mpd->periods[0]->asets[0]->representations[0]->total_duration);

    return 0;
}


static void mpd_output_segment_template(AVIOContext *out, int duration, int timescale)
{
    avio_printf(out, "\t\t\t<SegmentTemplate ");
    avio_printf(out, "initialization=\"init.mp4\" ");
    avio_printf(out, "media=\"seg-$Number$.m4s\" ");
    avio_printf(out, "startNumber=\"1\" ");
    avio_printf(out, "duration=\"%d\" ", duration); // TODO!!
    avio_printf(out, "timescale=\"%d\" />\n", timescale);

    // TODO: add segment timeline option
}

static void mpd_output_representation(AVIOContext *out, YPRepresentation *representation)
{
    avio_printf(out, "\t\t\t<Representation ");
    avio_printf(out, "id=\"%d\"  ", representation->id);
    avio_printf(out, "codecs=\"%s\" ", representation->codecs);
    avio_printf(out, "width=\"%d\" ", representation->width);
    avio_printf(out, "height=\"%d\" ", representation->height);
    avio_printf(out, "frameRate=\"%d/%d\" ", representation->avg_frame_rate.num, representation->avg_frame_rate.den);
    avio_printf(out, "bandwidth=\"%"PRId64"\" />\n", representation->bandwidth);
}

static void mpd_output_segments(void)
{
}

static int mpd_add_segment(YPIndexHandlerClass *self, YPInputStream *instream, char *filename, int64_t pos, int64_t size, double duration, int num)
{
    printf("segment: file: %s pos: %" PRId64 " size %" PRId64 " duration %.2f num %d\n",
            filename, pos, size, duration, num);
    YPMPD *mpd = (YPMPD*) self->opaque;
    int ret = 0;
    unsigned int aset_id = instream->set_id;
    unsigned int rep_id = instream->stream_id;
    YPRepresentation* representation = mpd->periods[0]->asets[aset_id]->representations[rep_id];
    YPSegment *segment = (YPSegment *) malloc(sizeof(YPSegment));

    // TODO: handle malloc error

    strlcpy(segment->filename, filename, sizeof(segment->filename));
    segment->pos = pos;
    segment->size = size;
    segment->index_size = 0;
    segment->duration = duration;
    segment->num = num;
    segment->next = NULL;
   
    representation->nb_segments++;
    representation->total_duration += segment->duration;

    if (representation->segments == NULL) {
        representation->segments = segment;
        representation->last_segment = segment;
        return 0;
    }
   
    representation->last_segment->next = segment;
    representation->last_segment = segment;
    return 0;
}

static int write_to_file(void)
{
    return 0;
}

YPIndexHandlerClass* yp_mpd_generator(void)
{
    YPIndexHandlerClass *ih = (YPIndexHandlerClass *) malloc(sizeof(YPIndexHandlerClass));

    if (ih) {
        ih->init = &mpd_init;
        ih->add_segment = &mpd_add_segment;
        ih->finalize = &mpd_finalize;
        return ih;
    }

    return NULL;
}

void yp_mpd_generator_free(YPIndexHandlerClass *self)
{
    if (self->opaque != NULL) {
        mpd_free((YPMPD*) self->opaque);
    }
    free(self);
}
