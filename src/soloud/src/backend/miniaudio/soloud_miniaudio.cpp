/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include <stdlib.h>

#include "soloud.h"

#if !defined(WITH_MINIAUDIO)

namespace SoLoud
{
    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer)
    {
        return NOT_IMPLEMENTED;
    }
}

#else

#define MINIAUDIO_IMPLEMENTATION
// // #define MA_NO_NULL
// #define MA_NO_DECODING
// #define MA_NO_WAV
// #define MA_NO_FLAC
// #define MA_NO_MP3
// #define MA_NO_AUTOINITIALIZATION
// #define MA_NO_VORBIS
// #define MA_NO_OPUS
// #define MA_NO_MIDI

// Seems that on miniaudio there is still an issue when uninitializing the device
// addressed by this issue: https://github.com/mackron/miniaudio/issues/466
// For me this happens using AAudio on android <= 10 (but not on Samsung Galaxy S9+).
// Disablig AAudio in favor of OpenSL is a workaround to prevent the crash.
#if defined(__ANDROID__) && (__ANDROID_API__ <= 29)
#define MA_NO_AAUDIO
#endif
// #define MA_DEBUG_OUTPUT
#include "miniaudio.h"
#include <math.h>

namespace SoLoud
{
    ma_device gDevice;
    SoLoud::Soloud *soloud;
    ma_context context; // Global context

    // Added by Marco Bavagnoli
    // Modified: Removed iOS-specific device restart on interruption end.
    void on_notification(const ma_device_notification* pNotification)
    {
        MA_ASSERT(pNotification != NULL);

        if (soloud->_stateChangedCallback == nullptr) {
            return;
        }

        switch (pNotification->type)
        {
            case ma_device_notification_type_started:
            {
                soloud->_stateChangedCallback(0);
            }
            break;

            case ma_device_notification_type_stopped:
            {
                soloud->_stateChangedCallback(1);
            } break;

            case ma_device_notification_type_rerouted:
            {
                soloud->_stateChangedCallback(2);
            } break;

            case ma_device_notification_type_interruption_began:
            {
                // When this is triggered the player goes into a paused state (internally by miniaudio/OS).
                // Notify Dart side. Manual handling (like fading) should be done in Dart based on this callback.
                soloud->_stateChangedCallback(3);
            } break;

            case ma_device_notification_type_interruption_ended:
            {
                // ** Removed iOS specific ma_device_start(&gDevice) **
                // The device is automatically stopped by CoreAudio on interruption begin.
                // We are removing the automatic restart here.
                // Restarting the audio playback logic should now be handled
                // manually in the Dart layer using the audio_session package
                // callbacks, likely by calling SoLoud's resume/play functions
                // after activating the audio session.
                // Should be nice to fade in from Dart side if desired.

                soloud->_stateChangedCallback(4); // Notify Dart interruption ended
            } break;

            case ma_device_notification_type_unlocked:
            {
                soloud->_stateChangedCallback(5);
            } break;

            default: break;
        }
    }

    void soloud_miniaudio_audiomixer(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
    {
        SoLoud::Soloud *soloud = (SoLoud::Soloud *)pDevice->pUserData;
        soloud->mix((float *)pOutput, frameCount);
    }

    static void soloud_miniaudio_deinit(SoLoud::Soloud *aSoloud)
    {
        ma_device_stop(&gDevice);
        ma_device_uninit(&gDevice);
        // Uninitialize the context when cleaning up
        ma_context_uninit(&context);
    }

    // Modified: Removed iOS-specific context configuration. Uses default context init.
    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels, void *pPlaybackInfos_id)
    {
        soloud = aSoloud;

        // ** Removed iOS specific ma_context_config initialization **
        // Initialize a default miniaudio context. We are removing the specific
        // iOS session category setup here, as it will be handled manually
        // via the audio_session package in Dart.
        ma_result result = ma_context_init(NULL, 0, NULL, &context);
        if (result != MA_SUCCESS) {
             // Log error: "Failed to initialize miniaudio context"
            return UNKNOWN_ERROR; // Or a more specific error code if available
        }

        // --- The code below remains largely the same, but now uses the default context ---

        // Note: Getting device info might still be useful for device selection,
        // but it's not strictly related to session management.
        // If needed, you can still get devices using the default context:
        // ma_device_info* pPlaybackInfos;
        // ma_uint32 playbackCount;
        // ma_device_info* pCaptureInfos;
        // ma_uint32 captureCount;
        // if (ma_context_get_devices(&context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount) != MA_SUCCESS) {
        //     ma_context_uninit(&context); // Clean up context on error
        //     return UNKNOWN_ERROR;
        // }


        // Configure and initialize the device
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        if (pPlaybackInfos_id != NULL) {
            deviceConfig.playback.pDeviceID = (ma_device_id*)pPlaybackInfos_id;
        }

        deviceConfig.periodSizeInFrames = aBuffer;
        deviceConfig.playback.format    = ma_format_f32;
        deviceConfig.playback.channels  = aChannels;
        deviceConfig.sampleRate         = aSamplerate;
        deviceConfig.dataCallback       = soloud_miniaudio_audiomixer;
        deviceConfig.pUserData          = (void *)aSoloud;

        // deviceConfig.aaudio.contentType = ma_aaudio_content_type_music;
        // deviceConfig.aaudio.usage = ma_aaudio_usage_media;
        // deviceConfig.aaudio.allowedCapturePolicy = ma_aaudio_allow_capture_by_all;

        if (aSoloud->_stateChangedCallback != nullptr) {
            deviceConfig.notificationCallback = on_notification;
        }

        // Initialize the device using the default context
        if (ma_device_init(&context, &deviceConfig, &gDevice) != MA_SUCCESS) {
            ma_context_uninit(&context); // Clean up context on error
            // Log error: "Failed to initialize miniaudio device"
            return UNKNOWN_ERROR;
        }

        // Note: It's important that postinit_internal uses the *actual* device parameters
        aSoloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames, aFlags, gDevice.playback.channels);
        aSoloud->mBackendCleanupFunc = soloud_miniaudio_deinit;

        // Start the device *after* SoLoud is posted internally
        if (ma_device_start(&gDevice) != MA_SUCCESS) {
             ma_device_uninit(&gDevice);
             ma_context_uninit(&context);
             // Log error: "Failed to start miniaudio device"
             return UNKNOWN_ERROR;
        }

        aSoloud->mBackendString = "MiniAudio";
        return 0; // Indicate success
    }

    result miniaudio_changeDevice_impl(void *pPlaybackInfos_id)
    {
        if (soloud == nullptr)
            return UNKNOWN_ERROR;

        // Stop and uninitialize the current device
        ma_device_stop(&gDevice); // Best practice to stop before uninit
        ma_device_uninit(&gDevice);

        // Reconfigure for the new device ID
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.pDeviceID = (ma_device_id *)pPlaybackInfos_id;
        config.periodSizeInFrames = soloud->mBufferSize;
        config.playback.format    = ma_format_f32;
        config.playback.channels  = soloud->mChannels;
        config.sampleRate         = soloud->mSamplerate;
        config.dataCallback       = soloud_miniaudio_audiomixer;
        config.pUserData          = (void *)soloud;
        // Re-add notification callback if it was set
        if (soloud->_stateChangedCallback != nullptr) {
             config.notificationCallback = on_notification;
        }

        // Initialize the new device using the *existing* default context
        // It's important *not* to re-initialize the context here.
        if (ma_device_init(&context, &config, &gDevice) != MA_SUCCESS)
        {
            // Log error: "Failed to initialize new audio device"
            // Context should still be valid, no need to uninit it here unless device init corrupts it
            return UNKNOWN_ERROR;
        }

        // Update Soloud's internal state if necessary (samplerate/buffer/channels might change with device)
        // It might be safer to call postinit_internal again if these can change
         soloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames, soloud->mFlags, gDevice.playback.channels);


        // Start the new device
        if (ma_device_start(&gDevice) != MA_SUCCESS) {
             ma_device_uninit(&gDevice);
             // Log error: "Failed to start new audio device"
             return UNKNOWN_ERROR;
        }

        return 0; // Indicate success
    }
};
#endif // WITH_MINIAUDIO