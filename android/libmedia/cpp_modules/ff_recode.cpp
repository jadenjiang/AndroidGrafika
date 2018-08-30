#include <media_common/media_struct.h>
#include "ff_recode.h"

namespace av{
    static void initOnce(JNIEnv *env){
        static bool init = false;
        if(!init){
            init = true;
            J4A_AVPacket_Class_Init(env);
            J4A_MediaInfo_Class_Init(env);
        }
    }

    int FFRecoder::initH264Bsf(const AVCodecParameters *codecpar){
        std::string  extensions = m_inputFormat->iformat->extensions;
        if(extensions.find("mp4")
           || extensions.find("mov")
           || extensions.find("m4a")){

        }
        else{
            return -1;
        }

        const AVBitStreamFilter * bsf = av_bsf_get_by_name("h264_mp4toannexb");
        if(bsf ==  nullptr){
            ALOGE(" find  h264_mp4toannexb  filter error");
            return -1;
        }
        AVBSFContext *bsf_ctx = nullptr;
        av_bsf_alloc(bsf, &bsf_ctx);
        if(bsf_ctx == nullptr){
            ALOGE("av_bsf_alloc error");
            return -1;
        }
        m_mp4H264_bsf.reset(bsf_ctx);
        avcodec_parameters_copy(m_mp4H264_bsf->par_in, codecpar);
        if(av_bsf_init(m_mp4H264_bsf.get()) < 0){
            m_mp4H264_bsf.reset();
            return -1;
        }
        return 0;
    }

    int FFRecoder::applyBitstream(AVPacket *packet) {
        int ret = 0;
        if(m_mp4H264_bsf == nullptr)
            return -1;
        ret = av_bsf_send_packet(m_mp4H264_bsf.get(), packet);
        if(ret < 0){
            ALOGE("av_bsf_send_packet error %d", ret);
            return ret;
        }
        av_packet_unref(packet);
        ret = av_bsf_receive_packet(m_mp4H264_bsf.get(), packet);
        if (ret < 0){
            ALOGE("av_bsf_receive_packet error %d", ret);
            return ret;
        }
        return ret;
    }



    int FFRecoder::openInputFormat(JNIEnv *env, const char *inputStr) {
        int ret = 0;
        initOnce(env);
        m_input = inputStr;
        AVFormatContext *ifmt_ctx = nullptr;
        ret = avformat_open_input(&ifmt_ctx, inputStr, nullptr, nullptr);
        if (ret < 0) {
            ALOGE("Could not open input file '%s'   %d", inputStr, ret);
            return ret;
        }

        if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
            avformat_close_input(&ifmt_ctx);
            ALOGE("Failed to retrieve input stream information");
            return ret;
        }
        m_inputFormat.reset(ifmt_ctx);
        ifmt_ctx = nullptr;
        getMediaInfo();

        for(int i = 0; i < m_inputFormat->nb_streams; i++){
            if(m_inputFormat->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
                initH264Bsf(m_inputFormat->streams[i]->codecpar);
                break;
            }
        }

        const AVCodecParameters *codecParameters = getCodecParameters(AVMEDIA_TYPE_AUDIO);
        if(codecParameters != nullptr){
            m_audio_decoder.reset(new AudioDecoder());
            m_audio_decoder->openCoder(codecParameters);

            m_audio_encoder.reset(new AudioEncoder());
            m_audio_encoder->openCoder(codecParameters);
        }

        return ret;
    }


    int FFRecoder::probeMediaInfo(JNIEnv *env, jobject object) {

        J4A_MediaInfo_setFiled_bitrate(env, object, m_stream_info.bitrate);
        J4A_MediaInfo_setFiled_duration(env, object, m_stream_info.duration);
        if(m_video_codecParam.width > 0 &&
           m_video_codecParam.height > 0){
            J4A_MediaInfo_setFiled_videoCodecID(env, object, m_video_codecParam.codec_id);
            J4A_MediaInfo_setFiled_width(env, object, m_video_codecParam.width);
            J4A_MediaInfo_setFiled_height(env, object, m_video_codecParam.height);
            J4A_MediaInfo_setFiled_framerate(env, object, m_video_codecParam.frame_rate);
        }

        if(m_audio_codecParam.sample_rate > 0 &&
           m_audio_codecParam.channels > 0){
            J4A_MediaInfo_setFiled_audioCodecID(env, object, m_audio_codecParam.codec_id);
            J4A_MediaInfo_setFiled_channels(env, object, m_audio_codecParam.channels);
            J4A_MediaInfo_setFiled_sampleRate(env, object, m_audio_codecParam.sample_rate);
            J4A_MediaInfo_setFiled_sampleDepth(env, object, m_audio_codecParam.sample_depth);
            J4A_MediaInfo_setFiled_frameSize(env, object, m_audio_codecParam.frame_size);
        }
        return 0;
    }


    int FFRecoder::openOutputFormat(JNIEnv *env, const char *outputStr){
        int ret = 0;
        m_muxer.reset(new AVMuxer());
        m_muxer->openOutputFormat(outputStr);

        if(m_video_codecParam.width > 0 &&
           m_video_codecParam.height > 0){
            const AVCodecParameters *src_codecpar = getCodecParameters(AVMEDIA_TYPE_VIDEO);
            AVCodecParameters *codecpar =  avcodec_parameters_alloc();
            codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            codecpar->width = src_codecpar->width;
            codecpar->height = src_codecpar->height;
            codecpar->sample_aspect_ratio = src_codecpar->sample_aspect_ratio;
            codecpar->codec_id = AV_CODEC_ID_H264;
            codecpar->format = AV_PIX_FMT_YUV420P;
            v_index = m_muxer->addStream(codecpar);
            avcodec_parameters_free(&codecpar);
        }

        if(m_audio_codecParam.sample_rate > 0 &&
           m_audio_codecParam.channels > 0){

            AVCodecParameters *codecpar =  avcodec_parameters_alloc();
            avcodec_parameters_from_context(codecpar, m_audio_encoder->getCodecContext());
            a_index = m_muxer->addStream(codecpar);
            avcodec_parameters_free(&codecpar);
        }
        if(m_spspps_buffer){
            m_muxer->setExternalData(m_spspps_buffer, m_spspps_buffer_size, v_index);
        }
        ret = m_muxer->writeHeader();
        return ret;

    }


    int FFRecoder::readPacket(JNIEnv *env, jobject packet_obj) {
        int ret = 0;
        std::unique_ptr<AVPacket, AVPacketDeleter> packet(av_packet_alloc());
        av_init_packet(packet.get());
        ret = av_read_frame(m_inputFormat.get(), packet.get());

        if(ret == AVERROR_EOF){
            while(true){
                if(drainAudioEncoder(nullptr) < 0)
                    break;
            }
            return AVERROR_EOF;
        }
        else if(ret < 0){
            return ret;
        }

        int stream_index = packet->stream_index;
        int mediaType = m_inputFormat->streams[stream_index]->codecpar->codec_type;
        AVRational stream_timebae = m_inputFormat->streams[stream_index]->time_base;

        if(mediaType == AVMEDIA_TYPE_VIDEO){
            av_packet_rescale_ts(packet.get(), stream_timebae, AV_TIME_BASE_Q);
            applyBitstream(packet.get());

//            if(packet->pts > 5000000){
//                ret = av_seek_frame(ifmt_ctx, -1, 0, 0);
//            }
//            static int64_t pts = 0;
//            pts += 33333;
//            packet->pts = packet->dts = pts;

            if (J4A_set_avpacket(env, packet_obj, packet->data, packet->size, packet->pts,
                                 packet->dts, mediaType) < 0) {
                ALOGE("GetDirectBufferAddress error");
                return -1;
            }

            return 0;
        }

        if (J4A_set_avpacket(env, packet_obj, nullptr, 0, 0, 0, mediaType) < 0) {
            ALOGE("GetDirectBufferAddress error");
            return -1;
        }
        if(mediaType == AVMEDIA_TYPE_AUDIO && m_get_spspps){
//            static int64_t pts = 0;
//            pts += packet->duration;
//            packet->pts = packet->dts = pts;

            ret = recodeWriteAudio(packet.get());
        }

        return ret;
    }


    int FFRecoder::writePacket(JNIEnv *env, jobject packet_obj) {
        int ret = 0;
        int64_t pts  = J4A_AVPacket_getFiled_ptsUs(env, packet_obj);
        int64_t dts  = J4A_AVPacket_getFiled_dtsUs(env, packet_obj);

        jobject bytebuffer = J4A_AVPacket_getFiled_buffer(env, packet_obj);
        void    *buf_ptr  = env->GetDirectBufferAddress(bytebuffer);
        int     buffer_size = J4A_AVPacket_getFiled_bufferSize(env, packet_obj);
        int     buffer_offset = J4A_AVPacket_getFiled_bufferOffset(env, packet_obj);
        int     buffer_flags = J4A_AVPacket_getFiled_bufferFlags(env, packet_obj);
        std::unique_ptr<AVPacket, AVPacketDeleter> vpacket(av_packet_alloc());
        av_init_packet(vpacket.get());
        av_new_packet(vpacket.get(), buffer_size);
        vpacket->stream_index = v_index;
        vpacket->pts = pts;
        vpacket->dts = dts;
        vpacket->flags = buffer_flags;
        memcpy(vpacket->data, buf_ptr, buffer_size);

        if(!m_get_spspps){
            m_get_spspps = true;
            m_spspps_buffer = get_sps_pps(vpacket->data, buffer_size, m_spspps_buffer_size);
            m_muxer->setExternalData(m_spspps_buffer, m_spspps_buffer_size, v_index);
        }
        ret = m_muxer->writePacket(vpacket.get());

        J4A_DeleteLocalRef__p(env, &bytebuffer);
        return ret;
    }

    int FFRecoder::recodeWriteAudio(AVPacket *packet){
        int ret = 0;
        int stream_index = packet->stream_index;
        AVRational stream_timebae = m_inputFormat.get()->streams[stream_index]->time_base;
        av_packet_rescale_ts(packet, stream_timebae, m_audio_decoder->getCodecContext()->time_base);

        //decoder
        ret = m_audio_decoder->sendPacket(packet);
        if(ret < 0){
            ALOGE("avcodec_send_packet error");
            return ret;
        }

        std::unique_ptr<AVFrame, AVFrameDeleter> aframe(av_frame_alloc());
        ret = m_audio_decoder->receiveFrame(aframe.get());
        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            ret = 0;
        }
        else if( ret < 0){
            ALOGE("avcodec_receive_frame error");
        }
        else{
            aframe->pts = aframe->best_effort_timestamp;
            aframe->pts = av_rescale_q(aframe->pts, m_audio_decoder->getCodecContext()->time_base,
                                       m_audio_encoder->getCodecContext()->time_base);
            ret = drainAudioEncoder(aframe.get());
        }
        return ret;
    }

    int FFRecoder::drainAudioEncoder(const AVFrame *frame){
        int ret = 0;
        std::unique_ptr<AVPacket, AVPacketDeleter> aPacket(av_packet_alloc());
        if(frame != nullptr){
            ret = m_audio_encoder->sendFrame(frame);
            if(ret < 0){
                ALOGE("avcodec_send_frame error");
            }

            ret = m_audio_encoder->receivPacket(aPacket.get());

        }
        else{
            ret = m_audio_encoder->flushCodec(aPacket.get());
        }

        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            if(frame != nullptr)
                ret = 0;
        }
        else if( ret < 0){
            ALOGE("avcodec_receive_packet error");

        }else{
            aPacket->stream_index = a_index;
            av_packet_rescale_ts(aPacket.get(), m_audio_encoder->getCodecContext()->time_base, AV_TIME_BASE_Q);
            ret = m_muxer->writePacket(aPacket.get());
        }
        return ret;
    }

    int FFRecoder::getMediaInfo() {
        if (m_inputFormat == nullptr)
            return -1;
        AVFormatContext *ifmt_ctx = m_inputFormat.get();
        m_stream_info.duration = ifmt_ctx->duration;
        m_stream_info.bitrate = ifmt_ctx->bit_rate;
        m_stream_info.start_time = ifmt_ctx->start_time;
        for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
            AVStream *stream = ifmt_ctx->streams[i];
            AVMediaType mediaType = stream->codecpar->codec_type;
            if (mediaType == AVMEDIA_TYPE_VIDEO) {
                m_video_codecParam.codec_id = stream->codecpar->codec_id;
                m_video_codecParam.width = stream->codecpar->width;
                m_video_codecParam.height = stream->codecpar->height;
                m_video_codecParam.frame_rate = av_q2d(stream->avg_frame_rate);

            } else if (mediaType == AVMEDIA_TYPE_AUDIO) {
                m_audio_codecParam.codec_id = stream->codecpar->codec_id;
                m_audio_codecParam.channels = stream->codecpar->channels;
                m_audio_codecParam.sample_rate = stream->codecpar->sample_rate;
                m_audio_codecParam.sample_depth = stream->codecpar->bits_per_coded_sample;
                m_audio_codecParam.frame_size = stream->codecpar->frame_size;
            }
        }
        return 0;
    }


    const AVCodecParameters* FFRecoder::getCodecParameters(enum AVMediaType type){
        AVFormatContext *ifmt_ctx = m_inputFormat.get();
        for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
            AVStream *stream = ifmt_ctx->streams[i];
            AVMediaType mediaType = stream->codecpar->codec_type;
            if(mediaType == type){
                return stream->codecpar;
            }
        }
        return nullptr;
    }
}