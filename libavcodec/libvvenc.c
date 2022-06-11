
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"

#include "avcodec.h"
#include "encode.h"
#include "internal.h"

#include <vvenc/vvenc.h>

typedef struct {
    vvenc_config params;
    vvencEncoder* encoder;
    vvencYUVBuffer* yuvbuf;
    vvencAccessUnit* au;
    bool encDone;
} VVEnCContext;

#define VVENC_LOG_ERROR( ...) \
    { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return AVERROR(EINVAL); \
    }

#define VVENC_LOG_WARNING( ...) \
    { \
        av_log(avctx, AV_LOG_WARNING, __VA_ARGS__); \
    }

#define VVENC_LOG_INFO( ...) \
    { \
        av_log(avctx, AV_LOG_INFO, __VA_ARGS__); \
    }

#define VVENC_LOG_VERBOSE( ...) \
    { \
        av_log(avctx, AV_LOG_VERBOSE, __VA_ARGS__); \
    }

#define VVENC_LOG_DBG( ...) \
    { \
        av_log(avctx, AV_LOG_DEBUG, __VA_ARGS__); \
    }

static av_cold void ff_vvenc_log_callback(void *avctx, int level, const char *fmt, va_list args)
{
    vfprintf(level == 1 ? stderr : stdout, fmt, args);
}

static av_cold void ff_vvenc_expand_bytes(uint8_t* src, int16_t* dst, int64_t n)
{
    for(int64_t i = 0; i < n; i++) {
        dst[i] = (int16_t)src[i];
    }
}

static av_cold int ff_vvenc_encode_init(AVCodecContext *avctx)
{
    VVEnCContext *q = avctx->priv_data;
    vvenc_init_default (&q->params, avctx->width, avctx->height, avctx->framerate.num, avctx->bit_rate, avctx->global_quality, VVENC_FASTER);

    q->params.m_verbosity = VVENC_DETAILS;
    if     ( av_log_get_level() >= AV_LOG_DEBUG )   q->params.m_verbosity = VVENC_DETAILS;
    else if( av_log_get_level() >= AV_LOG_VERBOSE ) q->params.m_verbosity = VVENC_INFO;     // VVDEC_INFO will output per picture info
    else if( av_log_get_level() >= AV_LOG_INFO )    q->params.m_verbosity = VVENC_WARNING;  // AV_LOG_INFO is ffmpeg default
    else q->params.m_verbosity = VVENC_SILENT;

    q->params.m_msgFnc = &ff_vvenc_log_callback;

    q->params.m_internChromaFormat = VVENC_CHROMA_420;
    if(avctx->pix_fmt == AV_PIX_FMT_YUV420P) {
        q->params.m_outputBitDepth[0] = q->params.m_internalBitDepth[0] = q->params.m_inputBitDepth[0] = 8;
    }
    else if(avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
        q->params.m_outputBitDepth[0] = q->params.m_internalBitDepth[0] = q->params.m_inputBitDepth[0] = 10;
    }

    if(avctx->thread_count > 0) {
        q->params.m_numThreads = avctx->thread_count;
    }
    else {
        q->params.m_numThreads = -1;
    }

    q->params.m_FrameScale = avctx->framerate.den;
    q->params.m_TicksPerSecond = 90000;

    vvenc_init_config_parameter(&q->params);

    q->encoder = vvenc_encoder_create();
    if(vvenc_encoder_open(q->encoder, &q->params)) {
        return -1;
    }

    q->yuvbuf = vvenc_YUVBuffer_alloc();
    vvenc_YUVBuffer_alloc_buffer(q->yuvbuf, q->params.m_internChromaFormat, q->params.m_SourceWidth, q->params.m_SourceHeight);

    q->au = vvenc_accessUnit_alloc();
    vvenc_accessUnit_alloc_payload(q->au, 2 * q->params.m_SourceWidth * q->params.m_SourceHeight + 1024);

    return 0;
}

static av_cold int ff_vvenc_encode_frame(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet_ptr)
{
    int pktSize;
    VVEnCContext *q = avctx->priv_data;

    for(int i = 0; i < 3; i++) {
        if(avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
            memcpy(q->yuvbuf->planes[i].ptr, frame->data[i], q->yuvbuf->planes[i].height * frame->linesize[i]);
        }
        else if (avctx->pix_fmt == AV_PIX_FMT_YUV420P) {
            ff_vvenc_expand_bytes(frame->data[i], q->yuvbuf->planes[i].ptr, q->yuvbuf->planes[i].height * frame->linesize[i]);
        }
    }

    q->yuvbuf->sequenceNumber = avctx->frame_number;
    q->yuvbuf->cts            = av_rescale_q(frame->pts, av_make_q(1, q->params.m_TicksPerSecond), avctx->time_base);
    q->yuvbuf->ctsValid       = true;

    if(vvenc_encode(q->encoder, q->yuvbuf, q->au, &q->encDone)) {
        return -1;
    }

    pktSize = q->au->payloadUsedSize;
    if (pktSize > 0) {
        ff_get_encode_buffer(avctx, avpkt, pktSize, 0);

        avpkt->dts = av_rescale_q(q->au->dts, av_make_q(1, q->params.m_TicksPerSecond), avctx->time_base);
        avpkt->pts = av_rescale_q(q->au->cts, av_make_q(1, q->params.m_TicksPerSecond), avctx->time_base);

        if (q->au->refPic) {
          avpkt->flags = AV_PKT_FLAG_KEY;
        }

        memcpy(avpkt->data, q->au->payload, pktSize);
        *got_packet_ptr = 1;
    }

    return 0;
}

static av_cold int ff_vvenc_encode_close(AVCodecContext *avctx)
{
    VVEnCContext *q = avctx->priv_data;

    vvenc_encoder_close(q->encoder);
    vvenc_YUVBuffer_free(q->yuvbuf, 1);
    vvenc_accessUnit_free(q->au, 1);

    return 0;
}

AVCodec ff_libvvenc_encoder = {
  	.name           = "libvvenc",
		.long_name      = "H.266 / VVC Encoder VVenC",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VVC,
    .priv_data_size = sizeof(VVEnCContext),
    .init           = ff_vvenc_encode_init,
    .encode2        = ff_vvenc_encode_frame,
    .close          = ff_vvenc_encode_close,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P,
                                                    AV_PIX_FMT_YUV420P10LE,
                                                    AV_PIX_FMT_NONE  },
    .capabilities    = AV_CODEC_CAP_OTHER_THREADS,
    .caps_internal   = FF_CODEC_CAP_AUTO_THREADS,
    .wrapper_name    = "libvvenc",
};
