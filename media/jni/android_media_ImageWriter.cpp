/*
 * Copyright 2015 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "ImageWriter_JNI"
#include "android_media_Utils.h"

#include <utils/Log.h>
#include <utils/String8.h>

#include <gui/IProducerListener.h>
#include <gui/Surface.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_runtime/android_view_Surface.h>
#include <jni.h>
#include <JNIHelp.h>

#include <stdint.h>
#include <inttypes.h>

#define IMAGE_BUFFER_JNI_ID           "mNativeBuffer"
#define IMAGE_FORMAT_UNKNOWN          0 // This is the same value as ImageFormat#UNKNOWN.
// ----------------------------------------------------------------------------

using namespace android;

static struct {
    jmethodID postEventFromNative;
    jfieldID mWriterFormat;
} gImageWriterClassInfo;

static struct {
    jfieldID mNativeBuffer;
    jfieldID mNativeFenceFd;
    jfieldID mPlanes;
} gSurfaceImageClassInfo;

static struct {
    jclass clazz;
    jmethodID ctor;
} gSurfacePlaneClassInfo;

// ----------------------------------------------------------------------------

class JNIImageWriterContext : public BnProducerListener {
public:
    JNIImageWriterContext(JNIEnv* env, jobject weakThiz, jclass clazz);

    virtual ~JNIImageWriterContext();

    // Implementation of IProducerListener, used to notify the ImageWriter that the consumer
    // has returned a buffer and it is ready for ImageWriter to dequeue.
    virtual void onBufferReleased();

    void setProducer(const sp<Surface>& producer) { mProducer = producer; }
    Surface* getProducer() { return mProducer.get(); }

    void setBufferFormat(int format) { mFormat = format; }
    int getBufferFormat() { return mFormat; }

    void setBufferWidth(int width) { mWidth = width; }
    int getBufferWidth() { return mWidth; }

    void setBufferHeight(int height) { mHeight = height; }
    int getBufferHeight() { return mHeight; }

private:
    static JNIEnv* getJNIEnv(bool* needsDetach);
    static void detachJNI();

    sp<Surface> mProducer;
    jobject mWeakThiz;
    jclass mClazz;
    int mFormat;
    int mWidth;
    int mHeight;
};

JNIImageWriterContext::JNIImageWriterContext(JNIEnv* env, jobject weakThiz, jclass clazz) :
    mWeakThiz(env->NewGlobalRef(weakThiz)),
    mClazz((jclass)env->NewGlobalRef(clazz)),
    mFormat(0),
    mWidth(-1),
    mHeight(-1) {
}

JNIImageWriterContext::~JNIImageWriterContext() {
    ALOGV("%s", __FUNCTION__);
    bool needsDetach = false;
    JNIEnv* env = getJNIEnv(&needsDetach);
    if (env != NULL) {
        env->DeleteGlobalRef(mWeakThiz);
        env->DeleteGlobalRef(mClazz);
    } else {
        ALOGW("leaking JNI object references");
    }
    if (needsDetach) {
        detachJNI();
    }

    mProducer.clear();
}

JNIEnv* JNIImageWriterContext::getJNIEnv(bool* needsDetach) {
    ALOGV("%s", __FUNCTION__);
    LOG_ALWAYS_FATAL_IF(needsDetach == NULL, "needsDetach is null!!!");
    *needsDetach = false;
    JNIEnv* env = AndroidRuntime::getJNIEnv();
    if (env == NULL) {
        JavaVMAttachArgs args = {JNI_VERSION_1_4, NULL, NULL};
        JavaVM* vm = AndroidRuntime::getJavaVM();
        int result = vm->AttachCurrentThread(&env, (void*) &args);
        if (result != JNI_OK) {
            ALOGE("thread attach failed: %#x", result);
            return NULL;
        }
        *needsDetach = true;
    }
    return env;
}

void JNIImageWriterContext::detachJNI() {
    ALOGV("%s", __FUNCTION__);
    JavaVM* vm = AndroidRuntime::getJavaVM();
    int result = vm->DetachCurrentThread();
    if (result != JNI_OK) {
        ALOGE("thread detach failed: %#x", result);
    }
}

void JNIImageWriterContext::onBufferReleased() {
    ALOGV("%s: buffer released", __FUNCTION__);
    bool needsDetach = false;
    JNIEnv* env = getJNIEnv(&needsDetach);
    if (env != NULL) {
        // Detach the buffer every time when a buffer consumption is done,
        // need let this callback give a BufferItem, then only detach if it was attached to this
        // Writer. Do the detach unconditionally for opaque format now. see b/19977520
        if (mFormat == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            sp<Fence> fence;
            sp<GraphicBuffer> buffer;
            ALOGV("%s: One buffer is detached", __FUNCTION__);
            mProducer->detachNextBuffer(&buffer, &fence);
        }

        env->CallStaticVoidMethod(mClazz, gImageWriterClassInfo.postEventFromNative, mWeakThiz);
    } else {
        ALOGW("onBufferReleased event will not posted");
    }

    if (needsDetach) {
        detachJNI();
    }
}

// ----------------------------------------------------------------------------

extern "C" {

// -------------------------------Private method declarations--------------

static void Image_setNativeContext(JNIEnv* env, jobject thiz,
        sp<GraphicBuffer> buffer, int fenceFd);
static void Image_getNativeContext(JNIEnv* env, jobject thiz,
        GraphicBuffer** buffer, int* fenceFd);
static void Image_unlockIfLocked(JNIEnv* env, jobject thiz);

// --------------------------ImageWriter methods---------------------------------------

static void ImageWriter_classInit(JNIEnv* env, jclass clazz) {
    ALOGV("%s:", __FUNCTION__);
    jclass imageClazz = env->FindClass("android/media/ImageWriter$WriterSurfaceImage");
    LOG_ALWAYS_FATAL_IF(imageClazz == NULL,
            "can't find android/media/ImageWriter$WriterSurfaceImage");
    gSurfaceImageClassInfo.mNativeBuffer = env->GetFieldID(
            imageClazz, IMAGE_BUFFER_JNI_ID, "J");
    LOG_ALWAYS_FATAL_IF(gSurfaceImageClassInfo.mNativeBuffer == NULL,
            "can't find android/media/ImageWriter$WriterSurfaceImage.%s", IMAGE_BUFFER_JNI_ID);

    gSurfaceImageClassInfo.mNativeFenceFd = env->GetFieldID(
            imageClazz, "mNativeFenceFd", "I");
    LOG_ALWAYS_FATAL_IF(gSurfaceImageClassInfo.mNativeFenceFd == NULL,
            "can't find android/media/ImageWriter$WriterSurfaceImage.mNativeFenceFd");

    gSurfaceImageClassInfo.mPlanes = env->GetFieldID(
            imageClazz, "mPlanes", "[Landroid/media/ImageWriter$WriterSurfaceImage$SurfacePlane;");
    LOG_ALWAYS_FATAL_IF(gSurfaceImageClassInfo.mPlanes == NULL,
            "can't find android/media/ImageWriter$WriterSurfaceImage.mPlanes");

    gImageWriterClassInfo.postEventFromNative = env->GetStaticMethodID(
            clazz, "postEventFromNative", "(Ljava/lang/Object;)V");
    LOG_ALWAYS_FATAL_IF(gImageWriterClassInfo.postEventFromNative == NULL,
                        "can't find android/media/ImageWriter.postEventFromNative");

    gImageWriterClassInfo.mWriterFormat = env->GetFieldID(
            clazz, "mWriterFormat", "I");
    LOG_ALWAYS_FATAL_IF(gImageWriterClassInfo.mWriterFormat == NULL,
                        "can't find android/media/ImageWriter.mWriterFormat");

    jclass planeClazz = env->FindClass("android/media/ImageWriter$WriterSurfaceImage$SurfacePlane");
    LOG_ALWAYS_FATAL_IF(planeClazz == NULL, "Can not find SurfacePlane class");
    // FindClass only gives a local reference of jclass object.
    gSurfacePlaneClassInfo.clazz = (jclass) env->NewGlobalRef(planeClazz);
    gSurfacePlaneClassInfo.ctor = env->GetMethodID(gSurfacePlaneClassInfo.clazz, "<init>",
            "(Landroid/media/ImageWriter$WriterSurfaceImage;IILjava/nio/ByteBuffer;)V");
    LOG_ALWAYS_FATAL_IF(gSurfacePlaneClassInfo.ctor == NULL,
            "Can not find SurfacePlane constructor");
}

static jlong ImageWriter_init(JNIEnv* env, jobject thiz, jobject weakThiz, jobject jsurface,
        jint maxImages, jint userFormat) {
    status_t res;

    ALOGV("%s: maxImages:%d", __FUNCTION__, maxImages);

    sp<Surface> surface(android_view_Surface_getSurface(env, jsurface));
    if (surface == NULL) {
        jniThrowException(env,
                "java/lang/IllegalArgumentException",
                "The surface has been released");
        return 0;
     }
    sp<IGraphicBufferProducer> bufferProducer = surface->getIGraphicBufferProducer();

    jclass clazz = env->GetObjectClass(thiz);
    if (clazz == NULL) {
        jniThrowRuntimeException(env, "Can't find android/graphics/ImageWriter");
        return 0;
    }
    sp<JNIImageWriterContext> ctx(new JNIImageWriterContext(env, weakThiz, clazz));

    sp<Surface> producer = new Surface(bufferProducer, /*controlledByApp*/false);
    ctx->setProducer(producer);
    /**
     * NATIVE_WINDOW_API_CPU isn't a good choice here, as it makes the bufferQueue not connectable
     * after disconnect. MEDIA or CAMERA are treated the same internally. The producer listener
     * will be cleared after disconnect call.
     */
    producer->connect(/*api*/NATIVE_WINDOW_API_CAMERA, /*listener*/ctx);
    jlong nativeCtx = reinterpret_cast<jlong>(ctx.get());

    // Get the dimension and format of the producer.
    sp<ANativeWindow> anw = producer;
    int32_t width, height, surfaceFormat;
    if ((res = anw->query(anw.get(), NATIVE_WINDOW_WIDTH, &width)) != OK) {
        ALOGE("%s: Query Surface width failed: %s (%d)", __FUNCTION__, strerror(-res), res);
        jniThrowRuntimeException(env, "Failed to query Surface width");
        return 0;
    }
    ctx->setBufferWidth(width);

    if ((res = anw->query(anw.get(), NATIVE_WINDOW_HEIGHT, &height)) != OK) {
        ALOGE("%s: Query Surface height failed: %s (%d)", __FUNCTION__, strerror(-res), res);
        jniThrowRuntimeException(env, "Failed to query Surface height");
        return 0;
    }
    ctx->setBufferHeight(height);

    // Query surface format if no valid user format is specified, otherwise, override surface format
    // with user format.
    if (userFormat == IMAGE_FORMAT_UNKNOWN) {
        if ((res = anw->query(anw.get(), NATIVE_WINDOW_FORMAT, &surfaceFormat)) != OK) {
            ALOGE("%s: Query Surface format failed: %s (%d)", __FUNCTION__, strerror(-res), res);
            jniThrowRuntimeException(env, "Failed to query Surface format");
            return 0;
        }
    } else {
        surfaceFormat = userFormat;
    }
    ctx->setBufferFormat(surfaceFormat);
    env->SetIntField(thiz,
            gImageWriterClassInfo.mWriterFormat, reinterpret_cast<jint>(surfaceFormat));

    if (!isFormatOpaque(surfaceFormat)) {
        res = native_window_set_usage(anw.get(), GRALLOC_USAGE_SW_WRITE_OFTEN);
        if (res != OK) {
            ALOGE("%s: Configure usage %08x for format %08x failed: %s (%d)",
                  __FUNCTION__, static_cast<unsigned int>(GRALLOC_USAGE_SW_WRITE_OFTEN),
                  surfaceFormat, strerror(-res), res);
            jniThrowRuntimeException(env, "Failed to SW_WRITE_OFTEN configure usage");
            return 0;
        }
    }

    int minUndequeuedBufferCount = 0;
    res = anw->query(anw.get(),
                NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBufferCount);
    if (res != OK) {
        ALOGE("%s: Query producer undequeued buffer count failed: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        jniThrowRuntimeException(env, "Query producer undequeued buffer count failed");
        return 0;
     }

    size_t totalBufferCount = maxImages + minUndequeuedBufferCount;
    res = native_window_set_buffer_count(anw.get(), totalBufferCount);
    if (res != OK) {
        ALOGE("%s: Set buffer count failed: %s (%d)", __FUNCTION__, strerror(-res), res);
        jniThrowRuntimeException(env, "Set buffer count failed");
        return 0;
    }

    if (ctx != 0) {
        ctx->incStrong((void*)ImageWriter_init);
    }
    return nativeCtx;
}

static void ImageWriter_dequeueImage(JNIEnv* env, jobject thiz, jlong nativeCtx, jobject image) {
    ALOGV("%s", __FUNCTION__);
    JNIImageWriterContext* const ctx = reinterpret_cast<JNIImageWriterContext *>(nativeCtx);
    if (ctx == NULL || thiz == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "ImageWriterContext is not initialized");
        return;
    }

    sp<ANativeWindow> anw = ctx->getProducer();
    android_native_buffer_t *anb = NULL;
    int fenceFd = -1;
    status_t res = anw->dequeueBuffer(anw.get(), &anb, &fenceFd);
    if (res != OK) {
        ALOGE("%s: Dequeue buffer failed: %s (%d)", __FUNCTION__, strerror(-res), res);
        switch (res) {
            case NO_INIT:
                jniThrowException(env, "java/lang/IllegalStateException",
                    "Surface has been abandoned");
                break;
            default:
                // TODO: handle other error cases here.
                jniThrowRuntimeException(env, "dequeue buffer failed");
        }
        return;
    }
    // New GraphicBuffer object doesn't own the handle, thus the native buffer
    // won't be freed when this object is destroyed.
    sp<GraphicBuffer> buffer(GraphicBuffer::from(anb));

    // Note that:
    // 1. No need to lock buffer now, will only lock it when the first getPlanes() is called.
    // 2. Fence will be saved to mNativeFenceFd, and will consumed by lock/queue/cancel buffer
    //    later.
    // 3. need use lockAsync here, as it will handle the dequeued fence for us automatically.

    // Finally, set the native info into image object.
    Image_setNativeContext(env, image, buffer, fenceFd);
}

static void ImageWriter_close(JNIEnv* env, jobject thiz, jlong nativeCtx) {
    ALOGV("%s:", __FUNCTION__);
    JNIImageWriterContext* const ctx = reinterpret_cast<JNIImageWriterContext *>(nativeCtx);
    if (ctx == NULL || thiz == NULL) {
        // ImageWriter is already closed.
        return;
    }

    ANativeWindow* producer = ctx->getProducer();
    if (producer != NULL) {
        /**
         * NATIVE_WINDOW_API_CPU isn't a good choice here, as it makes the bufferQueue not
         * connectable after disconnect. MEDIA or CAMERA are treated the same internally.
         * The producer listener will be cleared after disconnect call.
         */
        status_t res = native_window_api_disconnect(producer, /*api*/NATIVE_WINDOW_API_CAMERA);
        /**
         * This is not an error. if client calling process dies, the window will
         * also die and all calls to it will return DEAD_OBJECT, thus it's already
         * "disconnected"
         */
        if (res == DEAD_OBJECT) {
            ALOGW("%s: While disconnecting ImageWriter from native window, the"
                    " native window died already", __FUNCTION__);
        } else if (res != OK) {
            ALOGE("%s: native window disconnect failed: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            jniThrowRuntimeException(env, "Native window disconnect failed");
            return;
        }
    }

    ctx->decStrong((void*)ImageWriter_init);
}

static void ImageWriter_cancelImage(JNIEnv* env, jobject thiz, jlong nativeCtx, jobject image) {
    ALOGV("%s", __FUNCTION__);
    JNIImageWriterContext* const ctx = reinterpret_cast<JNIImageWriterContext *>(nativeCtx);
    if (ctx == NULL || thiz == NULL) {
        ALOGW("ImageWriter#close called before Image#close, consider calling Image#close first");
        return;
    }

    sp<ANativeWindow> anw = ctx->getProducer();

    GraphicBuffer *buffer = NULL;
    int fenceFd = -1;
    Image_getNativeContext(env, image, &buffer, &fenceFd);
    if (buffer == NULL) {
        // Cancel an already cancelled image is harmless.
        return;
    }

    // Unlock the image if it was locked
    Image_unlockIfLocked(env, image);

    anw->cancelBuffer(anw.get(), buffer, fenceFd);

    Image_setNativeContext(env, image, NULL, -1);
}

static void ImageWriter_queueImage(JNIEnv* env, jobject thiz, jlong nativeCtx, jobject image,
        jlong timestampNs, jint left, jint top, jint right, jint bottom) {
    ALOGV("%s", __FUNCTION__);
    JNIImageWriterContext* const ctx = reinterpret_cast<JNIImageWriterContext *>(nativeCtx);
    if (ctx == NULL || thiz == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "ImageWriterContext is not initialized");
        return;
    }

    status_t res = OK;
    sp<ANativeWindow> anw = ctx->getProducer();

    GraphicBuffer *buffer = NULL;
    int fenceFd = -1;
    Image_getNativeContext(env, image, &buffer, &fenceFd);
    if (buffer == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Image is not initialized");
        return;
    }

    // Unlock image if it was locked.
    Image_unlockIfLocked(env, image);

    // Set timestamp
    ALOGV("timestamp to be queued: %" PRId64, timestampNs);
    res = native_window_set_buffers_timestamp(anw.get(), timestampNs);
    if (res != OK) {
        jniThrowRuntimeException(env, "Set timestamp failed");
        return;
    }

    // Set crop
    android_native_rect_t cropRect;
    cropRect.left = left;
    cropRect.top = top;
    cropRect.right = right;
    cropRect.bottom = bottom;
    res = native_window_set_crop(anw.get(), &cropRect);
    if (res != OK) {
        jniThrowRuntimeException(env, "Set crop rect failed");
        return;
    }

    // Finally, queue input buffer
    res = anw->queueBuffer(anw.get(), buffer, fenceFd);
    if (res != OK) {
        ALOGE("%s: Queue buffer failed: %s (%d)", __FUNCTION__, strerror(-res), res);
        switch (res) {
            case NO_INIT:
                jniThrowException(env, "java/lang/IllegalStateException",
                    "Surface has been abandoned");
                break;
            default:
                // TODO: handle other error cases here.
                jniThrowRuntimeException(env, "Queue input buffer failed");
        }
        return;
    }

    // Clear the image native context: end of this image's lifecycle in public API.
    Image_setNativeContext(env, image, NULL, -1);
}

static jint ImageWriter_attachAndQueueImage(JNIEnv* env, jobject thiz, jlong nativeCtx,
        jlong nativeBuffer, jint imageFormat, jlong timestampNs, jint left, jint top,
        jint right, jint bottom) {
    ALOGV("%s", __FUNCTION__);
    JNIImageWriterContext* const ctx = reinterpret_cast<JNIImageWriterContext *>(nativeCtx);
    if (ctx == NULL || thiz == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "ImageWriterContext is not initialized");
        return -1;
    }

    sp<Surface> surface = ctx->getProducer();
    status_t res = OK;
    if (isFormatOpaque(imageFormat) != isFormatOpaque(ctx->getBufferFormat())) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Trying to attach an opaque image into a non-opaque ImageWriter, or vice versa");
        return -1;
    }

    // Image is guaranteed to be from ImageReader at this point, so it is safe to
    // cast to BufferItem pointer.
    BufferItem* buffer = reinterpret_cast<BufferItem*>(nativeBuffer);
    if (buffer == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Image is not initialized or already closed");
        return -1;
    }

    // Step 1. Attach Image
    res = surface->attachBuffer(buffer->mGraphicBuffer.get());
    if (res != OK) {
        ALOGE("Attach image failed: %s (%d)", strerror(-res), res);
        switch (res) {
            case NO_INIT:
                jniThrowException(env, "java/lang/IllegalStateException",
                    "Surface has been abandoned");
                break;
            default:
                // TODO: handle other error cases here.
                jniThrowRuntimeException(env, "nativeAttachImage failed!!!");
        }
        return res;
    }
    sp < ANativeWindow > anw = surface;

    // Step 2. Set timestamp and crop. Note that we do not need unlock the image because
    // it was not locked.
    ALOGV("timestamp to be queued: %" PRId64, timestampNs);
    res = native_window_set_buffers_timestamp(anw.get(), timestampNs);
    if (res != OK) {
        jniThrowRuntimeException(env, "Set timestamp failed");
        return res;
    }

    android_native_rect_t cropRect;
    cropRect.left = left;
    cropRect.top = top;
    cropRect.right = right;
    cropRect.bottom = bottom;
    res = native_window_set_crop(anw.get(), &cropRect);
    if (res != OK) {
        jniThrowRuntimeException(env, "Set crop rect failed");
        return res;
    }

    // Step 3. Queue Image.
    res = anw->queueBuffer(anw.get(), buffer->mGraphicBuffer.get(), /*fenceFd*/
            -1);
    if (res != OK) {
        ALOGE("%s: Queue buffer failed: %s (%d)", __FUNCTION__, strerror(-res), res);
        switch (res) {
            case NO_INIT:
                jniThrowException(env, "java/lang/IllegalStateException",
                    "Surface has been abandoned");
                break;
            default:
                // TODO: handle other error cases here.
                jniThrowRuntimeException(env, "Queue input buffer failed");
        }
        return res;
    }

    // Do not set the image native context. Since it would overwrite the existing native context
    // of the image that is from ImageReader, the subsequent image close will run into issues.

    return res;
}

// --------------------------Image methods---------------------------------------

static void Image_getNativeContext(JNIEnv* env, jobject thiz,
        GraphicBuffer** buffer, int* fenceFd) {
    ALOGV("%s", __FUNCTION__);
    if (buffer != NULL) {
        GraphicBuffer *gb = reinterpret_cast<GraphicBuffer *>
                  (env->GetLongField(thiz, gSurfaceImageClassInfo.mNativeBuffer));
        *buffer = gb;
    }

    if (fenceFd != NULL) {
        *fenceFd = reinterpret_cast<jint>(env->GetIntField(
                thiz, gSurfaceImageClassInfo.mNativeFenceFd));
    }
}

static void Image_setNativeContext(JNIEnv* env, jobject thiz,
        sp<GraphicBuffer> buffer, int fenceFd) {
    ALOGV("%s:", __FUNCTION__);
    GraphicBuffer* p = NULL;
    Image_getNativeContext(env, thiz, &p, /*fenceFd*/NULL);
    if (buffer != 0) {
        buffer->incStrong((void*)Image_setNativeContext);
    }
    if (p) {
        p->decStrong((void*)Image_setNativeContext);
    }
    env->SetLongField(thiz, gSurfaceImageClassInfo.mNativeBuffer,
            reinterpret_cast<jlong>(buffer.get()));

    env->SetIntField(thiz, gSurfaceImageClassInfo.mNativeFenceFd, reinterpret_cast<jint>(fenceFd));
}

static void Image_unlockIfLocked(JNIEnv* env, jobject thiz) {
    ALOGV("%s", __FUNCTION__);
    GraphicBuffer* buffer;
    Image_getNativeContext(env, thiz, &buffer, NULL);
    if (buffer == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Image is not initialized");
        return;
    }

    // Is locked?
    bool isLocked = false;
    jobject planes = NULL;
    if (!isFormatOpaque(buffer->getPixelFormat())) {
        planes = env->GetObjectField(thiz, gSurfaceImageClassInfo.mPlanes);
    }
    isLocked = (planes != NULL);
    if (isLocked) {
        // no need to use fence here, as we it will be consumed by either cancel or queue buffer.
        status_t res = buffer->unlock();
        if (res != OK) {
            jniThrowRuntimeException(env, "unlock buffer failed");
        }
        ALOGV("Successfully unlocked the image");
    }
}

static jint Image_getWidth(JNIEnv* env, jobject thiz) {
    ALOGV("%s", __FUNCTION__);
    GraphicBuffer* buffer;
    Image_getNativeContext(env, thiz, &buffer, NULL);
    if (buffer == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Image is not initialized");
        return -1;
    }

    return buffer->getWidth();
}

static jint Image_getHeight(JNIEnv* env, jobject thiz) {
    ALOGV("%s", __FUNCTION__);
    GraphicBuffer* buffer;
    Image_getNativeContext(env, thiz, &buffer, NULL);
    if (buffer == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Image is not initialized");
        return -1;
    }

    return buffer->getHeight();
}

static jint Image_getFormat(JNIEnv* env, jobject thiz) {
    ALOGV("%s", __FUNCTION__);
    GraphicBuffer* buffer;
    Image_getNativeContext(env, thiz, &buffer, NULL);
    if (buffer == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Image is not initialized");
        return 0;
    }

    // ImageWriter doesn't support data space yet, assuming it is unknown.
    PublicFormat publicFmt = android_view_Surface_mapHalFormatDataspaceToPublicFormat(
            buffer->getPixelFormat(), HAL_DATASPACE_UNKNOWN);
    return static_cast<jint>(publicFmt);
}

static void Image_setFenceFd(JNIEnv* env, jobject thiz, int fenceFd) {
    ALOGV("%s:", __FUNCTION__);
    env->SetIntField(thiz, gSurfaceImageClassInfo.mNativeFenceFd, reinterpret_cast<jint>(fenceFd));
}

static void Image_getLockedImage(JNIEnv* env, jobject thiz, LockedImage *image) {
    ALOGV("%s", __FUNCTION__);
    GraphicBuffer* buffer;
    int fenceFd = -1;
    Image_getNativeContext(env, thiz, &buffer, &fenceFd);
    if (buffer == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Image is not initialized");
        return;
    }

    // ImageWriter doesn't use crop by itself, app sets it, use the no crop version.
    const Rect noCrop(buffer->width, buffer->height);
    status_t res = lockImageFromBuffer(
            buffer, GRALLOC_USAGE_SW_WRITE_OFTEN, noCrop, fenceFd, image);
    // Clear the fenceFd as it is already consumed by lock call.
    Image_setFenceFd(env, thiz, /*fenceFd*/-1);
    if (res != OK) {
        jniThrowExceptionFmt(env, "java/lang/RuntimeException",
                "lock buffer failed for format 0x%x",
                buffer->getPixelFormat());
        return;
    }

    ALOGV("%s: Successfully locked the image", __FUNCTION__);
    // crop, transform, scalingMode, timestamp, and frameNumber should be set by producer,
    // and we don't set them here.
}

static void Image_getLockedImageInfo(JNIEnv* env, LockedImage* buffer, int idx,
        int32_t writerFormat, uint8_t **base, uint32_t *size, int *pixelStride, int *rowStride) {
    ALOGV("%s", __FUNCTION__);

    status_t res = getLockedImageInfo(buffer, idx, writerFormat, base, size,
            pixelStride, rowStride);
    if (res != OK) {
        jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException",
                             "Pixel format: 0x%x is unsupported", buffer->flexFormat);
    }
}

static jobjectArray Image_createSurfacePlanes(JNIEnv* env, jobject thiz,
        int numPlanes, int writerFormat) {
    ALOGV("%s: create SurfacePlane array with size %d", __FUNCTION__, numPlanes);
    int rowStride, pixelStride;
    uint8_t *pData;
    uint32_t dataSize;
    jobject byteBuffer;

    int format = Image_getFormat(env, thiz);
    if (isFormatOpaque(format) && numPlanes > 0) {
        String8 msg;
        msg.appendFormat("Format 0x%x is opaque, thus not writable, the number of planes (%d)"
                " must be 0", format, numPlanes);
        jniThrowException(env, "java/lang/IllegalArgumentException", msg.string());
        return NULL;
    }

    jobjectArray surfacePlanes = env->NewObjectArray(numPlanes, gSurfacePlaneClassInfo.clazz,
            /*initial_element*/NULL);
    if (surfacePlanes == NULL) {
        jniThrowRuntimeException(env, "Failed to create SurfacePlane arrays,"
                " probably out of memory");
        return NULL;
    }
    if (isFormatOpaque(format)) {
        return surfacePlanes;
    }

    // Buildup buffer info: rowStride, pixelStride and byteBuffers.
    LockedImage lockedImg = LockedImage();
    Image_getLockedImage(env, thiz, &lockedImg);

    // Create all SurfacePlanes
    PublicFormat publicWriterFormat = static_cast<PublicFormat>(writerFormat);
    writerFormat = android_view_Surface_mapPublicFormatToHalFormat(publicWriterFormat);
    for (int i = 0; i < numPlanes; i++) {
        Image_getLockedImageInfo(env, &lockedImg, i, writerFormat,
                &pData, &dataSize, &pixelStride, &rowStride);
        byteBuffer = env->NewDirectByteBuffer(pData, dataSize);
        if ((byteBuffer == NULL) && (env->ExceptionCheck() == false)) {
            jniThrowException(env, "java/lang/IllegalStateException",
                    "Failed to allocate ByteBuffer");
            return NULL;
        }

        // Finally, create this SurfacePlane.
        jobject surfacePlane = env->NewObject(gSurfacePlaneClassInfo.clazz,
                    gSurfacePlaneClassInfo.ctor, thiz, rowStride, pixelStride, byteBuffer);
        env->SetObjectArrayElement(surfacePlanes, i, surfacePlane);
    }

    return surfacePlanes;
}

} // extern "C"

// ----------------------------------------------------------------------------

static JNINativeMethod gImageWriterMethods[] = {
    {"nativeClassInit",         "()V",                        (void*)ImageWriter_classInit },
    {"nativeInit",              "(Ljava/lang/Object;Landroid/view/Surface;II)J",
                                                              (void*)ImageWriter_init },
    {"nativeClose",              "(J)V",                      (void*)ImageWriter_close },
    {"nativeAttachAndQueueImage", "(JJIJIIII)I",          (void*)ImageWriter_attachAndQueueImage },
    {"nativeDequeueInputImage", "(JLandroid/media/Image;)V",  (void*)ImageWriter_dequeueImage },
    {"nativeQueueInputImage",   "(JLandroid/media/Image;JIIII)V",  (void*)ImageWriter_queueImage },
    {"cancelImage",             "(JLandroid/media/Image;)V",   (void*)ImageWriter_cancelImage },
};

static JNINativeMethod gImageMethods[] = {
    {"nativeCreatePlanes",      "(II)[Landroid/media/ImageWriter$WriterSurfaceImage$SurfacePlane;",
                                                              (void*)Image_createSurfacePlanes },
    {"nativeGetWidth",         "()I",                         (void*)Image_getWidth },
    {"nativeGetHeight",        "()I",                         (void*)Image_getHeight },
    {"nativeGetFormat",        "()I",                         (void*)Image_getFormat },
};

int register_android_media_ImageWriter(JNIEnv *env) {

    int ret1 = AndroidRuntime::registerNativeMethods(env,
                   "android/media/ImageWriter", gImageWriterMethods, NELEM(gImageWriterMethods));

    int ret2 = AndroidRuntime::registerNativeMethods(env,
                   "android/media/ImageWriter$WriterSurfaceImage", gImageMethods, NELEM(gImageMethods));

    return (ret1 || ret2);
}
