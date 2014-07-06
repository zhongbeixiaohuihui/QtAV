/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2014 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "QtAV/VideoDecoderFFmpegHW.h"
#include "private/VideoDecoderFFmpegHW_p.h"
#include "utils/GPUMemCopy.h"
#include "QtAV/QtAV_Compat.h"
#include "prepost.h"
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus
#include <libavcodec/vda.h>
#ifdef __cplusplus
}
#endif //__cplusplus
#include <VideoDecodeAcceleration/VDADecoder.h>

// TODO: add to QtAV_Compat.h?
// FF_API_PIX_FMT
#ifdef PixelFormat
#undef PixelFormat
#endif

namespace QtAV {

class VideoDecoderVDAPrivate;
class VideoDecoderVDA : public VideoDecoderFFmpegHW
{
    Q_OBJECT
    DPTR_DECLARE_PRIVATE(VideoDecoderVDA)
    Q_PROPERTY(bool SSE4 READ SSE4 WRITE setSSE4)
public:
    VideoDecoderVDA();
    virtual ~VideoDecoderVDA();
    virtual VideoDecoderId id() const;
    virtual QString description() const;
    virtual VideoFrame frame();
    // QObject properties
    void setSSE4(bool y);
    bool SSE4() const;
};

extern VideoDecoderId VideoDecoderId_VDA;
FACTORY_REGISTER_ID_AUTO(VideoDecoder, VDA, "VDA")

void RegisterVideoDecoderVDA_Man()
{
    FACTORY_REGISTER_ID_MAN(VideoDecoder, VDA, "VDA")
}


class VideoDecoderVDAPrivate : public VideoDecoderFFmpegHWPrivate
{
public:
    VideoDecoderVDAPrivate()
        : VideoDecoderFFmpegHWPrivate()
        , copy_uswc(false)
    {
        description = "VDA";
        va_pixfmt = QTAV_PIX_FMT_C(VDA_VLD);
        memset(&hw_ctx, 0, sizeof(hw_ctx));
    }
    ~VideoDecoderVDAPrivate() {}
    virtual bool open();
    virtual void close();

    virtual bool setup(void **hwctx, AVPixelFormat *chroma, int w, int h);
    virtual bool getBuffer(void **opaque, uint8_t **data);
    virtual void releaseBuffer(void *opaque, uint8_t *data);

    struct vda_context  hw_ctx;
    // false for not intel gpu. my test result is intel gpu is supper fast and lower cpu usage if use optimized uswc copy. but nv is worse.
    bool copy_uswc;
    GPUMemCopy gpu_mem;
};

struct vda_error {
    int  code;
    const char *str;
};

static const struct vda_error vda_errors[] = {
    { kVDADecoderHardwareNotSupportedErr,
        "Hardware doesn't support accelerated decoding" },
    { kVDADecoderFormatNotSupportedErr,
        "Hardware doesn't support requested output format" },
    { kVDADecoderConfigurationError,
        "Invalid configuration provided to VDADecoderCreate" },
    { kVDADecoderDecoderFailedErr,
        "Generic error returned by the decoder layer. The cause can range from"
        " VDADecoder finding errors in the bitstream to another application"
        " using VDA at the moment. Only one application can use VDA at a"
        " givent time." },
    { 0, NULL },
};

static const char* vda_err_str(int err)
{
    for (int i = 0; vda_errors[i].code; ++i) {
        if (vda_errors[i].code != err)
            continue;
        return vda_errors[i].str;
    }
    return 0;
}

VideoDecoderVDA::VideoDecoderVDA()
    : VideoDecoderFFmpegHW(*new VideoDecoderVDAPrivate())
{
}

VideoDecoderVDA::~VideoDecoderVDA()
{
}

VideoDecoderId VideoDecoderVDA::id() const
{
    return VideoDecoderId_VDA;
}

QString VideoDecoderVDA::description() const
{
    return "Video Decode Acceleration";
}

void VideoDecoderVDA::setSSE4(bool y)
{
    d_func().copy_uswc = y;
}

bool VideoDecoderVDA::SSE4() const
{
    return d_func().copy_uswc;
}

VideoFrame VideoDecoderVDA::frame()
{
    DPTR_D(VideoDecoderVDA);
    CVPixelBufferRef cv_buffer = (CVPixelBufferRef)d.frame->data[3];
    if (!cv_buffer) {
        qDebug("Frame buffer is empty.");
        return VideoFrame();
    }
    if (CVPixelBufferGetDataSize(cv_buffer) <= 0) {
        qDebug("Empty frame buffer");
        return VideoFrame();
    }
    VideoFormat::PixelFormat pixfmt = VideoFormat::Format_Invalid;
    switch (d.hw_ctx.cv_pix_fmt_type) {
    case kCVPixelFormatType_420YpCbCr8Planar:
        pixfmt = VideoFormat::Format_YUV420P;
        break;
    case kCVPixelFormatType_422YpCbCr8:
        pixfmt = VideoFormat::Format_UYVY;
        break;
    default:
        break;
    }
    if (pixfmt == VideoFormat::Format_Invalid) {
        qWarning("unsupported vda pixel format: %#x", d.hw_ctx.cv_pix_fmt_type);
        return VideoFrame();
    }
    const VideoFormat fmt(pixfmt);
    uint8_t *src[3];
    int pitch[3];
    int plane_h[3];
    int yuv_size = 0;
    CVPixelBufferLockBaseAddress(cv_buffer, 0);
    for (int i = 0; i <fmt.planeCount(); ++i) {
        src[i] = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(cv_buffer, i);
        pitch[i] = CVPixelBufferGetBytesPerRowOfPlane(cv_buffer, i);
        plane_h[i] = i == 0 ? d.height : fmt.chromaHeight(d.height);
        yuv_size += pitch[i] * plane_h[i];
    }
    CVPixelBufferUnlockBaseAddress(cv_buffer, 0);
    CVPixelBufferRelease(cv_buffer);
    VideoFrame frame;
    if (d.copy_uswc && d.gpu_mem.isReady()) {
        // additional 15 bytes to ensure 16 bytes aligned
        QByteArray buf(15 + yuv_size, 0);
        const int offset_16 = (16 - ((uintptr_t)buf.data() & 0x0f)) & 0x0f;
        // plane 1, 2... is aligned?
        uchar* plane_ptr = (uchar*)buf.data() + offset_16;
        QVector<uchar*> dst(fmt.planeCount(), 0);
        for (int i = 0; i < dst.size(); ++i) {
            dst[i] = plane_ptr;
            // TODO: add VideoFormat::planeWidth/Height() ?
            plane_ptr += pitch[i] * plane_h[i];
            d.gpu_mem.copyFrame(src[i], dst[i], pitch[i], plane_h[i], pitch[i]);
            //memcpy(dst[i], src[i], pitch[i]*plane_h[i]);
        }
        frame = VideoFrame(buf, d.width, d.height, fmt);
        frame.setBits(dst);
        frame.setBytesPerLine(pitch);
    } else {
        frame = VideoFrame(d.width, d.height, fmt); //d.hw_ctx.width
        frame.setBits(src);
        frame.setBytesPerLine(pitch);
        //frame = frame.clone();
    }
    return frame;
}

bool VideoDecoderVDAPrivate::setup(void **pp_hw_ctx, AVPixelFormat *pi_chroma, int w, int h)
{
    if (hw_ctx.width == w && hw_ctx.height == h && hw_ctx.decoder) {
        width = w;
        height = h;
        *pp_hw_ctx = &hw_ctx;
        *pi_chroma = va_pixfmt;
        return true;
    }
    if (hw_ctx.decoder) {
        ff_vda_destroy_decoder(&hw_ctx);
        if (copy_uswc) {
            gpu_mem.cleanCache();
        }
    } else {
        memset(&hw_ctx, 0, sizeof(hw_ctx));
        hw_ctx.format = 'avc1';
        // TODO: other formats, such as kCVPixelFormatType_422YpCbCr8
        hw_ctx.cv_pix_fmt_type = kCVPixelFormatType_420YpCbCr8Planar;
    }
    /* Setup the libavcodec hardware context */
    *pp_hw_ctx = &hw_ctx;
    *pi_chroma = va_pixfmt;
    hw_ctx.width = w;
    hw_ctx.height = h;
    width = w;
    height = h;
    /* create the decoder */
    int status = ff_vda_create_decoder(&hw_ctx, codec_ctx->extradata, codec_ctx->extradata_size);
    if (status) {
        qWarning("Failed to create decoder (%i): %s", status, vda_err_str(status));
        return false;
    }
    qDebug("VDA decoder created");
    if (copy_uswc) {
        if (!gpu_mem.initCache(hw_ctx.width)) {
            // only set by user (except disabling copy_uswc for non-intel gpu)
            //copy_uswc = false;
            //disable_derive = true;
        }
    }
    return true;
}

bool VideoDecoderVDAPrivate::getBuffer(void **opaque, uint8_t **data)
{
    Q_UNUSED(data);
    //qDebug("%s @%d data=%p", __PRETTY_FUNCTION__, __LINE__, *data);
    // FIXME: why *data == 0?
    //*data = (uint8_t *)1; // dummy
    Q_UNUSED(opaque);
    return true;
}

void VideoDecoderVDAPrivate::releaseBuffer(void *opaque, uint8_t *data)
{
    Q_UNUSED(opaque);
    // released in getBuffer?
    CVPixelBufferRef cv_buffer = (CVPixelBufferRef)data;
    if (!cv_buffer)
        return;
    qDebug("release buffer");
    CVPixelBufferRelease(cv_buffer);
}

bool VideoDecoderVDAPrivate::open()
{
    qDebug("opening VDA module");
    if (codec_ctx->codec_id != AV_CODEC_ID_H264) {
        qWarning("input codec (%s) isn't H264, canceling VDA decoding", codec_ctx->codec_name);
        return false;
    }
#if 0
    if (!codec_ctx->extradata || codec_ctx->extradata_size < 7) {
        qWarning("VDA requires extradata.");
        return false;
    }
#endif
    return true;
}

void VideoDecoderVDAPrivate::close()
{
    restore(); //IMPORTANT. can not call restore in dtor because ctx is 0 there
    qDebug("destroying VDA decoder");
    ff_vda_destroy_decoder(&hw_ctx) ;
    if (copy_uswc) {
        gpu_mem.cleanCache();
    }
}

} //namespace QtAV
#include "VideoDecoderVDA.moc"
