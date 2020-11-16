/*
 * Copyright (C) 2015, The CyanogenMod Project
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

#define LOG_NDEBUG 0
#define LOG_PARAMETERS

#define LOG_TAG "Camera2Wrapper"
#include <log/log.h>

#include "CameraWrapper.h"
#include "Camera2Wrapper.h"

using namespace android;

// Wrapper specific parameters names
static const char KEY_SUPPORTED_ISO_MODES[] = "iso-values";
static const char KEY_ISO_MODE[] = "iso";

// Wrapper Sony specific parameters names
static const char KEY_SONY_IMAGE_STABILISER_VALUES[] = "sony-is-values";
static const char KEY_SONY_IMAGE_STABILISER[] = "sony-is";
static const char KEY_SONY_VIDEO_STABILISER[] = "sony-vs";
static const char KEY_SONY_VIDEO_STABILISER_VALUES[] = "sony-vs-values";
static const char KEY_SONY_VIDEO_HDR[] = "sony-video-hdr";
static const char KEY_SONY_VIDEO_HDR_VALUES[] = "sony-video-hdr-values";
static const char KEY_SONY_ISO_AVAIL_MODES[] = "sony-iso-values";
static const char KEY_SONY_ISO_MODE[] = "sony-iso";
static const char KEY_SONY_AE_MODE_VALUES[] = "sony-ae-mode-values";
static const char KEY_SONY_AE_MODE[] = "sony-ae-mode";

// Wrapper Sony specific parameters values
static const char VALUE_SONY_ON[] = "on";
static const char VALUE_SONY_OFF[] = "off";
static const char VALUE_SONY_STILL_HDR[] = "on-still-hdr";
static const char VALUE_SONY_INTELLIGENT_ACTIVE[] = "on-intelligent-active";

typedef struct wrapper_camera2_device {
    camera_device_t base;
    int camera2_released;
    int id;
    camera_device_t *vendor;
} wrapper_camera2_device_t;

#define VENDOR_CALL(device, func, ...) ({ \
    wrapper_camera2_device_t *__wrapper_dev = (wrapper_camera2_device_t*) device; \
    __wrapper_dev->vendor->ops->func(__wrapper_dev->vendor, ##__VA_ARGS__); \
})

#define CAMERA_ID(device) (((wrapper_camera2_device_t *)(device))->id)

static camera_module_t *gVendorModule = 0;

static camera_notify_callback gUserNotifyCb = NULL;
static camera_data_callback gUserDataCb = NULL;
static camera_data_timestamp_callback gUserDataCbTimestamp = NULL;
static camera_request_memory gUserGetMemory = NULL;
static void *gUserCameraDevice = NULL;

void camera_notify_cb(int32_t msg_type, int32_t ext1, int32_t ext2, void *user) {
    gUserNotifyCb(msg_type, ext1, ext2, gUserCameraDevice);
}

void camera_data_cb(int32_t msg_type, const camera_memory_t *data, unsigned int index,
        camera_frame_metadata_t *metadata, void *user) {
    gUserDataCb(msg_type, data, index, metadata, gUserCameraDevice);
}

void camera_data_cb_timestamp(nsecs_t timestamp, int32_t msg_type,
        const camera_memory_t *data, unsigned index, void *user) {
    gUserDataCbTimestamp(timestamp, msg_type, data, index, gUserCameraDevice);
}

camera_memory_t* camera_get_memory(int fd, size_t buf_size,
        uint_t num_bufs, void *user) {
    return gUserGetMemory(fd, buf_size, num_bufs, gUserCameraDevice);
}

static int check_vendor_module()
{
    int rv = 0;
    ALOGV("%s", __FUNCTION__);

    if(gVendorModule)
        return 0;

    rv = hw_get_module_by_class("camera", "vendor", (const hw_module_t **)&gVendorModule);
    if (rv)
        ALOGE("failed to open vendor camera module");
    return rv;
}

void camera2_fixup_capability(CameraParameters *params)
{
    ALOGV("%s", __FUNCTION__);

    if (params->get(
            KEY_SONY_IMAGE_STABILISER_VALUES)) {
        const char *supportedIsModes = params->get(
                KEY_SONY_IMAGE_STABILISER_VALUES);

        if (strstr(supportedIsModes, VALUE_SONY_STILL_HDR) != NULL) {
            char buffer[512];
            const char *supportedSceneModes = params->get(
                    CameraParameters::KEY_SUPPORTED_SCENE_MODES);
            sprintf(buffer, "%s,hdr", supportedSceneModes);
            params->set(CameraParameters::KEY_SUPPORTED_SCENE_MODES,
                    buffer);
        }
    }
}

static char *camera2_fixup_getparams(int __attribute__((unused)) id,
    const char *settings)
{
    CameraParameters params;
    params.unflatten(String8(settings));

#if !LOG_NDEBUG && defined(LOG_PARAMETERS)
    ALOGV("%s: original parameters:", __FUNCTION__);
    params.dump();
#endif

    camera2_fixup_capability(&params);

    if (params.get(KEY_SONY_ISO_AVAIL_MODES)) {
        // fixup the iso mode list with those that are in the sony list
        const char *isoModeList = params.get(KEY_SONY_ISO_AVAIL_MODES);
        char buffer[255] = "ISO";
        size_t bufferPos = 3;
        for (size_t pos = 0; pos < strlen(isoModeList); pos++) {
            if (isoModeList[pos] != ',') {
                buffer[bufferPos++] = isoModeList[pos];
            } else {
                strcat(buffer, ",ISO");
                bufferPos += 4;
            }
        }
        strcat(buffer, ",auto");
        params.set(KEY_SUPPORTED_ISO_MODES, buffer);
    }

    if (params.get(KEY_SONY_IMAGE_STABILISER)) {
        const char *sony_is = params.get(KEY_SONY_IMAGE_STABILISER);
        if (strcmp(sony_is, VALUE_SONY_STILL_HDR) == 0) {
            // Scene mode is HDR then (see fixup_setparams)
            params.set(CameraParameters::KEY_SCENE_MODE, "hdr");
        }
    }

    if (params.get(KEY_SONY_VIDEO_HDR) &&
            params.get(KEY_SONY_VIDEO_HDR_VALUES)) {
        params.set("video-hdr-values", params.get(KEY_SONY_VIDEO_HDR_VALUES));
        params.set("video-hdr", params.get(KEY_SONY_VIDEO_HDR));
    }

    if (params.get(KEY_SONY_ISO_MODE)) {
        if (params.get(KEY_SONY_AE_MODE_VALUES)) {
            const char *aeMode = params.get(KEY_SONY_AE_MODE);
            if (strcmp(aeMode, "auto") == 0 ) {
                params.set(KEY_ISO_MODE, "auto");
                params.set("shutter-speed", "auto");
            } else if (strcmp(aeMode, "iso-prio") == 0) {
                char *isoVal = (char*)malloc(sizeof(char) * (3 + strlen(
                        params.get(KEY_SONY_ISO_MODE))));
                sprintf(isoVal, "ISO%s", params.get(KEY_SONY_ISO_MODE));
                params.set(KEY_ISO_MODE, isoVal);
                params.set("shutter-speed", "auto");
            } else if (strcmp(aeMode, "shutter-prio") == 0) {
                params.set(KEY_ISO_MODE, "auto");
                const char *shutterSpeed = params.get("sony-shutter-speed");
                if (shutterSpeed) {
                    params.set("shutter-speed",shutterSpeed);
                }
            } else if (strcmp(aeMode, "manual") == 0) {
                const char *shutterSpeed = params.get("sony-shutter-speed");
                if (shutterSpeed) {
                    params.set("shutter-speed",shutterSpeed);
                }
                char *isoVal = (char*)malloc(sizeof(char) * (3 + strlen(
                        params.get(KEY_SONY_ISO_MODE))));
                sprintf(isoVal, "ISO%s" ,params.get(KEY_SONY_ISO_MODE));
                params.set(KEY_ISO_MODE, isoVal);
            } else {
                params.set(KEY_ISO_MODE, "auto");
                params.set("shutter-speed", "auto");
            }
        }
    }

    String8 strParams = params.flatten();
    char *ret = strdup(strParams.string());

    ALOGV("%s: get parameters fixed up", __FUNCTION__);
    return ret;
}

static char *camera2_fixup_setparams(int __attribute__((unused)) id,
        const char *settings)
{
    CameraParameters params;
    params.unflatten(String8(settings));

    const char *shutterSpeed = params.get("shutter-speed");
    if (shutterSpeed) {
        if (strcmp(shutterSpeed, "auto") != 0) {
            params.set("sony-shutter-speed", shutterSpeed);
            params.set(KEY_SONY_AE_MODE, "shutter-prio");
        } else {
            const char *aeModes = params.get(KEY_SONY_AE_MODE_VALUES);
            if (strstr(aeModes, "auto") != NULL) {
                params.set(KEY_SONY_AE_MODE, "auto");
            }
        }
    }

    if (params.get(KEY_ISO_MODE)) {
        const char *isoMode = params.get(KEY_ISO_MODE);
        if (strcmp(isoMode, "auto") != 0) {
            params.set(KEY_SONY_ISO_MODE, isoMode + 3);
        }
        if (params.get(KEY_SONY_AE_MODE_VALUES)) {
            const char *aeModes = params.get(KEY_SONY_AE_MODE_VALUES);
            if (strcmp(isoMode, "auto") == 0) {
                if ((strstr(aeModes, "auto") != NULL) && (strcmp(params.get(
                        KEY_SONY_AE_MODE), "shutter-prio") != 0)) {
                    params.set(KEY_SONY_AE_MODE, "auto");
                }
            } else if (strstr(aeModes, "iso-prio") != NULL) {
                if (strcmp(params.get(KEY_SONY_AE_MODE),
                        "shutter-prio") == 0) {
                    params.set(KEY_SONY_AE_MODE, "manual");
                } else {
                    params.set(KEY_SONY_AE_MODE, "iso-prio");
                }
            }
        }
    }

    if (params.get(CameraParameters::KEY_SCENE_MODE)) {
        const char *sceneMode = params.get(
                CameraParameters::KEY_SCENE_MODE);
        if (strcmp(sceneMode, "hdr") == 0) {
            params.set(KEY_SONY_IMAGE_STABILISER, VALUE_SONY_STILL_HDR);
            params.set(CameraParameters::KEY_SCENE_MODE,
                    CameraParameters::SCENE_MODE_AUTO);
        } else {
            params.set(KEY_SONY_IMAGE_STABILISER, VALUE_SONY_ON);
        }
    }

    if (params.get(KEY_SONY_VIDEO_HDR) && params.get("video-hdr")) {
        params.set(KEY_SONY_VIDEO_HDR, params.get("video-hdr"));
    }

    if (params.get(CameraParameters::KEY_RECORDING_HINT) &&
            strcmp(params.get(CameraParameters::KEY_RECORDING_HINT),
            CameraParameters::TRUE) == 0) {
        if (params.get(KEY_SONY_VIDEO_STABILISER_VALUES) &&
                (strstr(params.get(KEY_SONY_VIDEO_STABILISER_VALUES),
                    VALUE_SONY_INTELLIGENT_ACTIVE) != NULL) ) {
            params.set(KEY_SONY_VIDEO_STABILISER,
                    VALUE_SONY_INTELLIGENT_ACTIVE);
        } else {
            params.set(KEY_SONY_VIDEO_STABILISER, VALUE_SONY_OFF);
        }
        params.set(KEY_SONY_IMAGE_STABILISER, VALUE_SONY_OFF);
    }

#if defined(LOG_PARAMETERS)
    params.dump();
#endif

    String8 strParams = params.flatten();
    char *ret = strdup(strParams.string());

    ALOGV("%s: fixed parameters:", __FUNCTION__);
    return ret;
}

/*******************************************************************
 * implementation of camera_device_ops functions
 *******************************************************************/

static int camera2_set_preview_window(struct camera_device *device,
        struct preview_stream_ops *window)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, set_preview_window, window);
}

static void camera2_set_callbacks(struct camera_device * device,
        camera_notify_callback notify_cb,
        camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory,
        void *user)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    gUserNotifyCb = notify_cb;
    gUserDataCb = data_cb;
    gUserDataCbTimestamp = data_cb_timestamp;
    gUserGetMemory = get_memory;
    gUserCameraDevice = user;

    VENDOR_CALL(device, set_callbacks, camera_notify_cb, camera_data_cb,
            camera_data_cb_timestamp, camera_get_memory, user);
}

static void camera2_enable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    VENDOR_CALL(device, enable_msg_type, msg_type);
}

static void camera2_disable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    VENDOR_CALL(device, disable_msg_type, msg_type);
}

static int camera2_msg_type_enabled(struct camera_device *device,
        int32_t msg_type)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return 0;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, msg_type_enabled, msg_type);
}

static int camera2_start_preview(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, start_preview);
}

static void camera2_stop_preview(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    VENDOR_CALL(device, stop_preview);
}

static int camera2_preview_enabled(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, preview_enabled);
}

static int camera2_store_meta_data_in_buffers(struct camera_device *device,
        int enable)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, store_meta_data_in_buffers, enable);
}

static int camera2_start_recording(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, start_recording);
}

static void camera2_stop_recording(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    VENDOR_CALL(device, stop_recording);
}

static int camera2_recording_enabled(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, recording_enabled);
}

static void camera2_release_recording_frame(struct camera_device *device,
        const void *opaque)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    VENDOR_CALL(device, release_recording_frame, opaque);
}

static int camera2_auto_focus(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;


    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, auto_focus);
}

static int camera2_cancel_auto_focus(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, cancel_auto_focus);
}

static int camera2_take_picture(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    // We safely avoid returning the exact result of VENDOR_CALL here. If ZSL
    // really bumps fast, take_picture will be called while a picture is
    // already being taken, leading to "picture already running" error,
    // crashing Gallery app. Afaik, there is no issue doing 0 (error appears
    // in logcat anyway if needed).
    return VENDOR_CALL(device, take_picture);
}

static int camera2_cancel_picture(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, cancel_picture);
}

static int camera2_set_parameters(struct camera_device *device,
        const char *params)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

#ifdef LOG_PARAMETERS
    ALOGV("%s: Before fixup:", __FUNCTION__);
    __android_log_write(ANDROID_LOG_VERBOSE, LOG_TAG, params);
#endif

    char *tmp = NULL;
    tmp = camera2_fixup_setparams(CAMERA_ID(device), params);

#ifdef LOG_PARAMETERS
    ALOGV("%s: After fixup:", __FUNCTION__);
    __android_log_write(ANDROID_LOG_VERBOSE, LOG_TAG, tmp);
#endif

    int ret = VENDOR_CALL(device, set_parameters, tmp);
    return ret;
}

static char *camera2_get_parameters(struct camera_device *device)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return NULL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    char *params = VENDOR_CALL(device, get_parameters);

#ifdef LOG_PARAMETERS
    ALOGV("%s: Before fixup:", __FUNCTION__);
    __android_log_write(ANDROID_LOG_VERBOSE, LOG_TAG, params);
#endif

    char *tmp = camera2_fixup_getparams(CAMERA_ID(device), params);
    VENDOR_CALL(device, put_parameters, params);
    params = tmp;

#ifdef LOG_PARAMETERS
    ALOGV("%s: After fixup:", __FUNCTION__);
    __android_log_write(ANDROID_LOG_VERBOSE, LOG_TAG, tmp);
#endif

    return params;
}

static void camera2_put_parameters(struct camera_device *device, char *params)
{
    ALOGV("%s", __FUNCTION__);
    if (params)
        free(params);

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));
}

static int camera2_send_command(struct camera_device *device,
        int32_t cmd, int32_t arg1, int32_t arg2)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, send_command, cmd, arg1, arg2);
}

static void camera2_release(struct camera_device *device)
{
    wrapper_camera2_device_t* wrapper_dev = NULL;

    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return;

    wrapper_dev = (wrapper_camera2_device_t*) device;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(wrapper_dev->vendor));

    VENDOR_CALL(device, release);

    wrapper_dev->camera2_released = true;
}

static int camera2_dump(struct camera_device *device, int fd)
{
    ALOGV("%s: camera_device %p", __FUNCTION__, device);
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera2_device_t*)device)->vendor));

    return VENDOR_CALL(device, dump, fd);
}


static int camera2_device_close(hw_device_t *device)
{
    int ret = 0;
    wrapper_camera2_device_t* wrapper_dev = NULL;

    ALOGV("%s: hw_device_t %p", __FUNCTION__, device);

    Mutex::Autolock lock(gCameraWrapperLock);

    if (!device) {
        ret = -EINVAL;
        goto done;
    }

    wrapper_dev = (wrapper_camera2_device_t*) device;

    if (!wrapper_dev->camera2_released) {
        ALOGI("%s: releasing camera device with id %d", __FUNCTION__,
                wrapper_dev->id);

        VENDOR_CALL(wrapper_dev, release);

        wrapper_dev->camera2_released = true;
    }

    ALOGI("%s: closing camera device with id %d", __FUNCTION__,
            wrapper_dev->id);

    wrapper_dev->vendor->common.close((hw_device_t *)wrapper_dev->vendor);

    if (wrapper_dev->base.ops)
        free(wrapper_dev->base.ops);

    free(wrapper_dev);

done:
    ALOGI("%s: camera device closed", __FUNCTION__);

    return ret;
}

/*******************************************************************
 * implementation of camera_module functions
 *******************************************************************/

/* open device handle to one of the cameras
 *
 * assume camera service will keep singleton of each camera
 * so this function will always only be called once per camera instance
 */

int camera2_device_open(const hw_module_t* module, const char* name,
                hw_device_t** device)
{
    int rv = 0;
    //int num_cameras = 0;
    int cameraid;
    wrapper_camera2_device_t* camera2_device = NULL;
    camera_device_ops_t* camera2_ops = NULL;

    android::Mutex::Autolock lock(gCameraWrapperLock);

    ALOGV("%s", __FUNCTION__);

    if (name != NULL) {
        if (check_vendor_module())
            return -EINVAL;

        cameraid = atoi(name);
        /*
        num_cameras = gVendorModule->get_number_of_cameras();

        if (cameraid > num_cameras) {
            ALOGE("camera service provided cameraid out of bounds, "
                    "cameraid = %d, num supported = %d",
                    cameraid, num_cameras);
            rv = -EINVAL;
            goto fail;
        }
        */

        camera2_device = (wrapper_camera2_device_t*)malloc(sizeof(*camera2_device));
        if (!camera2_device) {
            ALOGE("camera2_device allocation fail");
            rv = -ENOMEM;
            goto fail;
        }
        memset(camera2_device, 0, sizeof(*camera2_device));
        camera2_device->camera2_released = false;
        camera2_device->id = cameraid;

        rv = gVendorModule->open_legacy((const hw_module_t*)gVendorModule, name, CAMERA_DEVICE_API_VERSION_1_0, (hw_device_t**)&(camera2_device->vendor));
        if (rv)
        {
            ALOGE("vendor camera open fail");
            goto fail;
        }
        ALOGV("%s: got vendor camera device 0x%08X", __FUNCTION__, (uintptr_t)(camera2_device->vendor));

        camera2_ops = (camera_device_ops_t*)malloc(sizeof(*camera2_ops));
        if (!camera2_ops) {
            ALOGE("camera_ops allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        memset(camera2_ops, 0, sizeof(*camera2_ops));

        camera2_device->base.common.tag = HARDWARE_DEVICE_TAG;
        camera2_device->base.common.version = CAMERA_DEVICE_API_VERSION_1_0;
        camera2_device->base.common.module = (hw_module_t *)(module);
        camera2_device->base.common.close = camera2_device_close;
        camera2_device->base.ops = camera2_ops;

        camera2_ops->set_preview_window = camera2_set_preview_window;
        camera2_ops->set_callbacks = camera2_set_callbacks;
        camera2_ops->enable_msg_type = camera2_enable_msg_type;
        camera2_ops->disable_msg_type = camera2_disable_msg_type;
        camera2_ops->msg_type_enabled = camera2_msg_type_enabled;
        camera2_ops->start_preview = camera2_start_preview;
        camera2_ops->stop_preview = camera2_stop_preview;
        camera2_ops->preview_enabled = camera2_preview_enabled;
        camera2_ops->store_meta_data_in_buffers = camera2_store_meta_data_in_buffers;
        camera2_ops->start_recording = camera2_start_recording;
        camera2_ops->stop_recording = camera2_stop_recording;
        camera2_ops->recording_enabled = camera2_recording_enabled;
        camera2_ops->release_recording_frame = camera2_release_recording_frame;
        camera2_ops->auto_focus = camera2_auto_focus;
        camera2_ops->cancel_auto_focus = camera2_cancel_auto_focus;
        camera2_ops->take_picture = camera2_take_picture;
        camera2_ops->cancel_picture = camera2_cancel_picture;
        camera2_ops->set_parameters = camera2_set_parameters;
        camera2_ops->get_parameters = camera2_get_parameters;
        camera2_ops->put_parameters = camera2_put_parameters;
        camera2_ops->send_command = camera2_send_command;
        camera2_ops->release = camera2_release;
        camera2_ops->dump = camera2_dump;

        *device = &camera2_device->base.common;
    }

    return rv;

fail:
    if(camera2_device) {
        free(camera2_device);
        camera2_device = NULL;
    }
    if(camera2_ops) {
        free(camera2_ops);
        camera2_ops = NULL;
    }
    *device = NULL;
    return rv;
}
