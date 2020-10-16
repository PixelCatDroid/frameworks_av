/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#define LOG_TAG "VideoTrackTranscoder"

#include <android-base/logging.h>
#include <media/NdkCommon.h>
#include <media/VideoTrackTranscoder.h>
#include <utils/AndroidThreads.h>

namespace android {

// Check that the codec sample flags have the expected NDK meaning.
static_assert(SAMPLE_FLAG_CODEC_CONFIG == AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG,
              "Sample flag mismatch: CODEC_CONFIG");
static_assert(SAMPLE_FLAG_END_OF_STREAM == AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM,
              "Sample flag mismatch: END_OF_STREAM");
static_assert(SAMPLE_FLAG_PARTIAL_FRAME == AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME,
              "Sample flag mismatch: PARTIAL_FRAME");

// Color format defined by surface. (See MediaCodecInfo.CodecCapabilities#COLOR_FormatSurface.)
static constexpr int32_t kColorFormatSurface = 0x7f000789;
// Default key frame interval in seconds.
static constexpr float kDefaultKeyFrameIntervalSeconds = 1.0f;

template <typename T>
void VideoTrackTranscoder::BlockingQueue<T>::push(T const& value, bool front) {
    {
        std::scoped_lock lock(mMutex);
        if (mAborted) {
            return;
        }

        if (front) {
            mQueue.push_front(value);
        } else {
            mQueue.push_back(value);
        }
    }
    mCondition.notify_one();
}

template <typename T>
T VideoTrackTranscoder::BlockingQueue<T>::pop() {
    std::unique_lock lock(mMutex);
    while (mQueue.empty()) {
        mCondition.wait(lock);
    }
    T value = mQueue.front();
    mQueue.pop_front();
    return value;
}

// Note: Do not call if another thread might waiting in pop.
template <typename T>
void VideoTrackTranscoder::BlockingQueue<T>::abort() {
    std::scoped_lock lock(mMutex);
    mAborted = true;
    mQueue.clear();
}

// The CodecWrapper class is used to let AMediaCodec instances outlive the transcoder object itself
// by giving the codec a weak pointer to the transcoder. Codecs wrapped in this object are kept
// alive by the transcoder and the codec's outstanding buffers. Once the transcoder stops and all
// output buffers have been released by downstream components the codec will also be released.
class VideoTrackTranscoder::CodecWrapper {
public:
    CodecWrapper(AMediaCodec* codec, const std::weak_ptr<VideoTrackTranscoder>& transcoder)
          : mCodec(codec), mTranscoder(transcoder), mCodecStarted(false) {}
    ~CodecWrapper() {
        if (mCodecStarted) {
            AMediaCodec_stop(mCodec);
        }
        AMediaCodec_delete(mCodec);
    }

    AMediaCodec* getCodec() { return mCodec; }
    std::shared_ptr<VideoTrackTranscoder> getTranscoder() const { return mTranscoder.lock(); };
    void setStarted() { mCodecStarted = true; }

private:
    AMediaCodec* mCodec;
    std::weak_ptr<VideoTrackTranscoder> mTranscoder;
    bool mCodecStarted;
};

// Dispatch responses to codec callbacks onto the message queue.
struct AsyncCodecCallbackDispatch {
    static void onAsyncInputAvailable(AMediaCodec* codec, void* userdata, int32_t index) {
        VideoTrackTranscoder::CodecWrapper* wrapper =
                static_cast<VideoTrackTranscoder::CodecWrapper*>(userdata);
        if (auto transcoder = wrapper->getTranscoder()) {
            if (codec == transcoder->mDecoder) {
                transcoder->mCodecMessageQueue.push(
                        [transcoder, index] { transcoder->enqueueInputSample(index); });
            }
        }
    }

    static void onAsyncOutputAvailable(AMediaCodec* codec, void* userdata, int32_t index,
                                       AMediaCodecBufferInfo* bufferInfoPtr) {
        VideoTrackTranscoder::CodecWrapper* wrapper =
                static_cast<VideoTrackTranscoder::CodecWrapper*>(userdata);
        AMediaCodecBufferInfo bufferInfo = *bufferInfoPtr;
        if (auto transcoder = wrapper->getTranscoder()) {
            transcoder->mCodecMessageQueue.push([transcoder, index, codec, bufferInfo] {
                if (codec == transcoder->mDecoder) {
                    transcoder->transferBuffer(index, bufferInfo);
                } else if (codec == transcoder->mEncoder->getCodec()) {
                    transcoder->dequeueOutputSample(index, bufferInfo);
                }
            });
        }
    }

    static void onAsyncFormatChanged(AMediaCodec* codec, void* userdata, AMediaFormat* format) {
        VideoTrackTranscoder::CodecWrapper* wrapper =
                static_cast<VideoTrackTranscoder::CodecWrapper*>(userdata);
        if (auto transcoder = wrapper->getTranscoder()) {
            const char* kCodecName = (codec == transcoder->mDecoder ? "Decoder" : "Encoder");
            LOG(DEBUG) << kCodecName << " format changed: " << AMediaFormat_toString(format);
            if (codec == transcoder->mEncoder->getCodec()) {
                transcoder->mCodecMessageQueue.push(
                        [transcoder, format] { transcoder->updateTrackFormat(format); });
            }
        }
    }

    static void onAsyncError(AMediaCodec* codec, void* userdata, media_status_t error,
                             int32_t actionCode, const char* detail) {
        LOG(ERROR) << "Error from codec " << codec << ", userdata " << userdata << ", error "
                   << error << ", action " << actionCode << ", detail " << detail;
        VideoTrackTranscoder::CodecWrapper* wrapper =
                static_cast<VideoTrackTranscoder::CodecWrapper*>(userdata);
        if (auto transcoder = wrapper->getTranscoder()) {
            transcoder->mCodecMessageQueue.push(
                    [transcoder, error] {
                        transcoder->mStatus = error;
                        transcoder->mStopRequested = true;
                    },
                    true);
        }
    }
};

// static
std::shared_ptr<VideoTrackTranscoder> VideoTrackTranscoder::create(
        const std::weak_ptr<MediaTrackTranscoderCallback>& transcoderCallback) {
    return std::shared_ptr<VideoTrackTranscoder>(new VideoTrackTranscoder(transcoderCallback));
}

VideoTrackTranscoder::~VideoTrackTranscoder() {
    if (mDecoder != nullptr) {
        AMediaCodec_delete(mDecoder);
    }

    if (mSurface != nullptr) {
        ANativeWindow_release(mSurface);
    }
}

// Creates and configures the codecs.
media_status_t VideoTrackTranscoder::configureDestinationFormat(
        const std::shared_ptr<AMediaFormat>& destinationFormat) {
    static constexpr int32_t kDefaultBitrateMbps = 10 * 1000 * 1000;

    media_status_t status = AMEDIA_OK;

    if (destinationFormat == nullptr) {
        LOG(ERROR) << "Destination format is null, use passthrough transcoder";
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    AMediaFormat* encoderFormat = AMediaFormat_new();
    if (!encoderFormat || AMediaFormat_copy(encoderFormat, destinationFormat.get()) != AMEDIA_OK) {
        LOG(ERROR) << "Unable to copy destination format";
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    int32_t bitrate;
    if (!AMediaFormat_getInt32(encoderFormat, AMEDIAFORMAT_KEY_BIT_RATE, &bitrate)) {
        status = mMediaSampleReader->getEstimatedBitrateForTrack(mTrackIndex, &bitrate);
        if (status != AMEDIA_OK) {
            LOG(ERROR) << "Unable to estimate bitrate. Using default " << kDefaultBitrateMbps;
            bitrate = kDefaultBitrateMbps;
        }

        LOG(INFO) << "Configuring bitrate " << bitrate;
        AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    }

    float tmp;
    if (!AMediaFormat_getFloat(encoderFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, &tmp)) {
        AMediaFormat_setFloat(encoderFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
                              kDefaultKeyFrameIntervalSeconds);
    }
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatSurface);

    // Always encode without rotation. The rotation degree will be transferred directly to
    // MediaSampleWriter track format, and MediaSampleWriter will call AMediaMuxer_setOrientationHint.
    AMediaFormat_setInt32(encoderFormat, AMEDIAFORMAT_KEY_ROTATION, 0);

    mDestinationFormat = std::shared_ptr<AMediaFormat>(encoderFormat, &AMediaFormat_delete);

    // Create and configure the encoder.
    const char* destinationMime = nullptr;
    bool ok = AMediaFormat_getString(mDestinationFormat.get(), AMEDIAFORMAT_KEY_MIME,
                                     &destinationMime);
    if (!ok) {
        LOG(ERROR) << "Destination MIME type is required for transcoding.";
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    AMediaCodec* encoder = AMediaCodec_createEncoderByType(destinationMime);
    if (encoder == nullptr) {
        LOG(ERROR) << "Unable to create encoder for type " << destinationMime;
        return AMEDIA_ERROR_UNSUPPORTED;
    }
    mEncoder = std::make_shared<CodecWrapper>(encoder, shared_from_this());

    status = AMediaCodec_configure(mEncoder->getCodec(), mDestinationFormat.get(),
                                   NULL /* surface */, NULL /* crypto */,
                                   AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Unable to configure video encoder: " << status;
        return status;
    }

    status = AMediaCodec_createInputSurface(mEncoder->getCodec(), &mSurface);
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Unable to create an encoder input surface: %d" << status;
        return status;
    }

    // Create and configure the decoder.
    const char* sourceMime = nullptr;
    ok = AMediaFormat_getString(mSourceFormat.get(), AMEDIAFORMAT_KEY_MIME, &sourceMime);
    if (!ok) {
        LOG(ERROR) << "Source MIME type is required for transcoding.";
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    mDecoder = AMediaCodec_createDecoderByType(sourceMime);
    if (mDecoder == nullptr) {
        LOG(ERROR) << "Unable to create decoder for type " << sourceMime;
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    auto decoderFormat = std::shared_ptr<AMediaFormat>(AMediaFormat_new(), &AMediaFormat_delete);
    if (!decoderFormat ||
        AMediaFormat_copy(decoderFormat.get(), mSourceFormat.get()) != AMEDIA_OK) {
        LOG(ERROR) << "Unable to copy source format";
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    // Prevent decoder from overwriting frames that the encoder has not yet consumed.
    AMediaFormat_setInt32(decoderFormat.get(), TBD_AMEDIACODEC_PARAMETER_KEY_ALLOW_FRAME_DROP, 0);

    // Copy over configurations that apply to both encoder and decoder.
    static const AMediaFormatUtils::EntryCopier kEncoderEntriesToCopy[] = {
            ENTRY_COPIER2(AMEDIAFORMAT_KEY_OPERATING_RATE, Float, Int32),
            ENTRY_COPIER(AMEDIAFORMAT_KEY_PRIORITY, Int32),
    };
    const size_t entryCount = sizeof(kEncoderEntriesToCopy) / sizeof(kEncoderEntriesToCopy[0]);
    AMediaFormatUtils::CopyFormatEntries(mDestinationFormat.get(), decoderFormat.get(),
                                         kEncoderEntriesToCopy, entryCount);

    status = AMediaCodec_configure(mDecoder, decoderFormat.get(), mSurface, NULL /* crypto */,
                                   0 /* flags */);
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Unable to configure video decoder: " << status;
        return status;
    }

    // Configure codecs to run in async mode.
    AMediaCodecOnAsyncNotifyCallback asyncCodecCallbacks = {
            .onAsyncInputAvailable = AsyncCodecCallbackDispatch::onAsyncInputAvailable,
            .onAsyncOutputAvailable = AsyncCodecCallbackDispatch::onAsyncOutputAvailable,
            .onAsyncFormatChanged = AsyncCodecCallbackDispatch::onAsyncFormatChanged,
            .onAsyncError = AsyncCodecCallbackDispatch::onAsyncError};

    // Note: The decoder does not need its own wrapper because its lifetime is tied to the
    // transcoder. But the same callbacks are reused for decoder and encoder so we pass the encoder
    // wrapper as userdata here but never read the codec from it in the callback.
    status = AMediaCodec_setAsyncNotifyCallback(mDecoder, asyncCodecCallbacks, mEncoder.get());
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Unable to set decoder to async mode: " << status;
        return status;
    }

    status = AMediaCodec_setAsyncNotifyCallback(mEncoder->getCodec(), asyncCodecCallbacks,
                                                mEncoder.get());
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Unable to set encoder to async mode: " << status;
        return status;
    }

    return AMEDIA_OK;
}

void VideoTrackTranscoder::enqueueInputSample(int32_t bufferIndex) {
    media_status_t status = AMEDIA_OK;

    if (mEosFromSource) {
        return;
    }

    status = mMediaSampleReader->getSampleInfoForTrack(mTrackIndex, &mSampleInfo);
    if (status != AMEDIA_OK && status != AMEDIA_ERROR_END_OF_STREAM) {
        LOG(ERROR) << "Error getting next sample info: " << status;
        mStatus = status;
        return;
    }
    const bool endOfStream = (status == AMEDIA_ERROR_END_OF_STREAM);

    if (!endOfStream) {
        size_t bufferSize = 0;
        uint8_t* sourceBuffer = AMediaCodec_getInputBuffer(mDecoder, bufferIndex, &bufferSize);
        if (sourceBuffer == nullptr) {
            LOG(ERROR) << "Decoder returned a NULL input buffer.";
            mStatus = AMEDIA_ERROR_UNKNOWN;
            return;
        } else if (bufferSize < mSampleInfo.size) {
            LOG(ERROR) << "Decoder returned an input buffer that is smaller than the sample.";
            mStatus = AMEDIA_ERROR_UNKNOWN;
            return;
        }

        status = mMediaSampleReader->readSampleDataForTrack(mTrackIndex, sourceBuffer,
                                                            mSampleInfo.size);
        if (status != AMEDIA_OK) {
            LOG(ERROR) << "Unable to read next sample data. Aborting transcode.";
            mStatus = status;
            return;
        }
    } else {
        LOG(DEBUG) << "EOS from source.";
        mEosFromSource = true;
    }

    status = AMediaCodec_queueInputBuffer(mDecoder, bufferIndex, 0, mSampleInfo.size,
                                          mSampleInfo.presentationTimeUs, mSampleInfo.flags);
    if (status != AMEDIA_OK) {
        LOG(ERROR) << "Unable to queue input buffer for decode: " << status;
        mStatus = status;
        return;
    }
}

void VideoTrackTranscoder::transferBuffer(int32_t bufferIndex, AMediaCodecBufferInfo bufferInfo) {
    if (bufferIndex >= 0) {
        bool needsRender = bufferInfo.size > 0;
        AMediaCodec_releaseOutputBuffer(mDecoder, bufferIndex, needsRender);
    }

    if (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        LOG(DEBUG) << "EOS from decoder.";
        media_status_t status = AMediaCodec_signalEndOfInputStream(mEncoder->getCodec());
        if (status != AMEDIA_OK) {
            LOG(ERROR) << "SignalEOS on encoder returned error: " << status;
            mStatus = status;
        }
    }
}

void VideoTrackTranscoder::dequeueOutputSample(int32_t bufferIndex,
                                               AMediaCodecBufferInfo bufferInfo) {
    if (bufferIndex >= 0) {
        size_t sampleSize = 0;
        uint8_t* buffer =
                AMediaCodec_getOutputBuffer(mEncoder->getCodec(), bufferIndex, &sampleSize);

        MediaSample::OnSampleReleasedCallback bufferReleaseCallback =
                [encoder = mEncoder](MediaSample* sample) {
                    AMediaCodec_releaseOutputBuffer(encoder->getCodec(), sample->bufferId,
                                                    false /* render */);
                };

        std::shared_ptr<MediaSample> sample = MediaSample::createWithReleaseCallback(
                buffer, bufferInfo.offset, bufferIndex, bufferReleaseCallback);
        sample->info.size = bufferInfo.size;
        sample->info.flags = bufferInfo.flags;
        sample->info.presentationTimeUs = bufferInfo.presentationTimeUs;

        onOutputSampleAvailable(sample);
    } else if (bufferIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* newFormat = AMediaCodec_getOutputFormat(mEncoder->getCodec());
        LOG(DEBUG) << "Encoder output format changed: " << AMediaFormat_toString(newFormat);
    }

    if (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        LOG(DEBUG) << "EOS from encoder.";
        mEosFromEncoder = true;
    }
}

void VideoTrackTranscoder::updateTrackFormat(AMediaFormat* outputFormat) {
    if (mActualOutputFormat != nullptr) {
        LOG(WARNING) << "Ignoring duplicate format change.";
        return;
    }

    AMediaFormat* formatCopy = AMediaFormat_new();
    if (!formatCopy || AMediaFormat_copy(formatCopy, outputFormat) != AMEDIA_OK) {
        LOG(ERROR) << "Unable to copy outputFormat";
        AMediaFormat_delete(formatCopy);
        mStatus = AMEDIA_ERROR_INVALID_PARAMETER;
        return;
    }

    // Generate the actual track format for muxer based on the encoder output format,
    // since many vital information comes in the encoder format (eg. CSD).
    // Transfer necessary fields from the user-configured track format (derived from
    // source track format and user transcoding request) where needed.

    // Transfer SAR settings:
    // If mDestinationFormat has SAR set, it means the original source has SAR specified
    // at container level. This is supposed to override any SAR settings in the bitstream,
    // thus should always be transferred to the container of the transcoded file.
    int32_t sarWidth, sarHeight;
    if (AMediaFormat_getInt32(mSourceFormat.get(), AMEDIAFORMAT_KEY_SAR_WIDTH, &sarWidth) &&
        (sarWidth > 0) &&
        AMediaFormat_getInt32(mSourceFormat.get(), AMEDIAFORMAT_KEY_SAR_HEIGHT, &sarHeight) &&
        (sarHeight > 0)) {
        AMediaFormat_setInt32(formatCopy, AMEDIAFORMAT_KEY_SAR_WIDTH, sarWidth);
        AMediaFormat_setInt32(formatCopy, AMEDIAFORMAT_KEY_SAR_HEIGHT, sarHeight);
    }
    // Transfer DAR settings.
    int32_t displayWidth, displayHeight;
    if (AMediaFormat_getInt32(mSourceFormat.get(), AMEDIAFORMAT_KEY_DISPLAY_WIDTH, &displayWidth) &&
        (displayWidth > 0) &&
        AMediaFormat_getInt32(mSourceFormat.get(), AMEDIAFORMAT_KEY_DISPLAY_HEIGHT,
                              &displayHeight) &&
        (displayHeight > 0)) {
        AMediaFormat_setInt32(formatCopy, AMEDIAFORMAT_KEY_DISPLAY_WIDTH, displayWidth);
        AMediaFormat_setInt32(formatCopy, AMEDIAFORMAT_KEY_DISPLAY_HEIGHT, displayHeight);
    }

    // Transfer rotation settings.
    // Note that muxer itself doesn't take rotation from the track format. It requires
    // AMediaMuxer_setOrientationHint to set the rotation. Here we pass the rotation to
    // MediaSampleWriter using the track format. MediaSampleWriter will then call
    // AMediaMuxer_setOrientationHint as needed.
    int32_t rotation;
    if (AMediaFormat_getInt32(mSourceFormat.get(), AMEDIAFORMAT_KEY_ROTATION, &rotation) &&
        (rotation != 0)) {
        AMediaFormat_setInt32(formatCopy, AMEDIAFORMAT_KEY_ROTATION, rotation);
    }

    // Transfer track duration.
    // Preserve the source track duration by sending it to MediaSampleWriter.
    int64_t durationUs;
    if (AMediaFormat_getInt64(mSourceFormat.get(), AMEDIAFORMAT_KEY_DURATION, &durationUs) &&
        durationUs > 0) {
        AMediaFormat_setInt64(formatCopy, AMEDIAFORMAT_KEY_DURATION, durationUs);
    }

    // TODO: transfer other fields as required.

    mActualOutputFormat = std::shared_ptr<AMediaFormat>(formatCopy, &AMediaFormat_delete);

    notifyTrackFormatAvailable();
}

media_status_t VideoTrackTranscoder::runTranscodeLoop() {
    androidSetThreadPriority(0 /* tid (0 = current) */, ANDROID_PRIORITY_VIDEO);

    // Push start decoder and encoder as two messages, so that these are subject to the
    // stop request as well. If the session is cancelled (or paused) immediately after start,
    // we don't need to waste time start then stop the codecs.
    mCodecMessageQueue.push([this] {
        media_status_t status = AMediaCodec_start(mDecoder);
        if (status != AMEDIA_OK) {
            LOG(ERROR) << "Unable to start video decoder: " << status;
            mStatus = status;
        }
    });

    mCodecMessageQueue.push([this] {
        media_status_t status = AMediaCodec_start(mEncoder->getCodec());
        if (status != AMEDIA_OK) {
            LOG(ERROR) << "Unable to start video encoder: " << status;
            mStatus = status;
        }
        mEncoder->setStarted();
    });

    // Process codec events until EOS is reached, transcoding is stopped or an error occurs.
    while (!mStopRequested && !mEosFromEncoder && mStatus == AMEDIA_OK) {
        std::function<void()> message = mCodecMessageQueue.pop();
        message();
    }

    mCodecMessageQueue.abort();
    AMediaCodec_stop(mDecoder);

    // Return error if transcoding was stopped before it finished.
    if (mStopRequested && !mEosFromEncoder && mStatus == AMEDIA_OK) {
        mStatus = AMEDIA_ERROR_UNKNOWN;  // TODO: Define custom error codes?
    }

    return mStatus;
}

void VideoTrackTranscoder::abortTranscodeLoop() {
    // Push abort message to the front of the codec event queue.
    mCodecMessageQueue.push([this] { mStopRequested = true; }, true /* front */);
}

std::shared_ptr<AMediaFormat> VideoTrackTranscoder::getOutputFormat() const {
    return mActualOutputFormat;
}

}  // namespace android
