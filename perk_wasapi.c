/* TODO:
 * [ ] Expose internal state with platform specific header.
 * [ ] Right now we assume that all devices have audio engine that supports IEEE format.
 *     We need to make sure this is either true, or at least is true for newer versions of windows.
 *     Otherwise we will implement format conversions, since floats are nice to work with.
 * [ ] Figure out how to deal with multichannel output.
 */

#include "perk.h"

// TODO: Prune.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <processthreadsapi.h>

#include <stdio.h>


#define REFTIMES_PER_SEC  10000000

/* end me */
DEFINE_GUID(based_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(based_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(based_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(based_IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);


#define kill_on_fail(hr, msg)                                           \
do {                                                                    \
    if (FAILED(hr)) {                                                   \
        fprintf(stderr, "%s:%d Perk: %s\n", __FILE__, __LINE__, msg);  \
        exit(666);                                                      \
    }                                                                   \
} while(0)

#define die(msg) \
do { \
    fprintf(stderr, "%s:%d Perk: %s\n", __FILE__, __LINE__, msg);  \
    exit(666);                                                      \
while (0)

#define log(msg)                                                            \
do {                                                                        \
    fprintf(stderr, "%s:%d Log (perk): %s\n", __FILE__, __LINE__, msg);    \
} while (0)

#define log_on_fail(hr, msg)                                                        \
do {                                                                            \
    if (FAILED(hr)) {                                                           \
        fprintf(stderr, "%s:%d Log (audio): %s\n", __FILE__, __LINE__, msg);    \
    }                                                                           \
} while(0)


void perk_win32_print_waveformat(const WAVEFORMATEX *format)
{
    printf(
        "    .Format = {\n"
        "        .wFormatTag      = %s,\n"
        "        .nChannels       = %d,\n"
        "        .nSamplesPerSec  = %d,\n"
        "        .nAvgBytesPerSec = %d,\n"
        "        .nBlockAlign     = %d,\n"
        "        .wBitsPerSample  = %d,\n"
        "        .cbSize          = %d,\n"
        "    },\n"
        ,
        format->wFormatTag == WAVE_FORMAT_PCM ? "WAVE_FORMAT_PCM" : "WAVE_FORMAT_EXTENSIBLE",
        format->nChannels,
        format->nSamplesPerSec,
        format->nAvgBytesPerSec,
        format->nBlockAlign,
        format->wBitsPerSample,
        format->cbSize
    );
}

const char *perk_win32_sub_format_to_string(GUID sub_format)
{
    if (IsEqualGUID(&KSDATAFORMAT_SUBTYPE_PCM, &sub_format))
             return "KSDATAFORMAT_SUBTYPE_PCM";

    if (IsEqualGUID(&KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, &sub_format))
             return "KSDATAFORMAT_SUBTYPE_IEEE_FLOAT";

    if (IsEqualGUID(&KSDATAFORMAT_SUBTYPE_DRM, &sub_format))
             return "KSDATAFORMAT_SUBTYPE_DRM";

    if (IsEqualGUID(&KSDATAFORMAT_SUBTYPE_ALAW, &sub_format))
             return "KSDATAFORMAT_SUBTYPE_ALAW";

    if (IsEqualGUID(&KSDATAFORMAT_SUBTYPE_MULAW, &sub_format))
             return "KSDATAFORMAT_SUBTYPE_MULAW";

    if (IsEqualGUID(&KSDATAFORMAT_SUBTYPE_ADPCM, &sub_format))
             return "KSDATAFORMAT_SUBTYPE_ADPCM";

    return "???";
}

void perk_win32_print_waveformatextensible(const WAVEFORMATEXTENSIBLE *format)
{
    printf("WAVEFORMATEXTENSIBLE = {\n");

    perk_win32_print_waveformat(&format->Format);

    if (format->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        printf(
            "    .Samples = {\n"
            "        .wValidBitsPerSample = %d,\n"
            "        .wSamplesPerBlock    = %d,\n"
            "        .wReserved           = %d,\n"
            "    },\n"
            "    .dwChannelMask = 0x%x,\n"
            "    .SubFormat = %s,\n"
            "}\n"
            ,
            format->Samples.wValidBitsPerSample,
            format->Samples.wSamplesPerBlock,
            format->Samples.wReserved,
            format->dwChannelMask,
            perk_win32_sub_format_to_string(format->SubFormat)
        );
    }
}


typedef struct
{
    HANDLE audio_thread;
    DWORD audio_thread_id;

    HANDLE init_event;
    HANDLE start_event;

    IMMDeviceEnumerator *enumerator;

    IMMDevice *device;

    IAudioClient *audio_client;

    HANDLE callback_event;

    IAudioRenderClient *render_client;
} perk_wasapi_t;

perk_wasapi_t perk_wasapi = {0};


static volatile perk_format_info_t static_format_info;
static volatile perk_start_info_t  static_start_info;


DWORD WINAPI perk_wasapi_audio_thread(void *param)
{
    (void)param;

    if (!SetThreadPriority(perk_wasapi.audio_thread, THREAD_PRIORITY_HIGHEST)) {
        log("Failed to raise audio thread priority!\n");
    }

    HRESULT hr = CoInitializeEx(0, COINIT_SPEED_OVER_MEMORY);
    kill_on_fail(hr, "Failed to initialize COM!");

    hr = CoCreateInstance(&based_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &based_IID_IMMDeviceEnumerator, (void**)&perk_wasapi.enumerator);
    kill_on_fail(hr, "Failed to instantiate DeviceEnumerator!");

    hr = perk_wasapi.enumerator->lpVtbl->GetDefaultAudioEndpoint(perk_wasapi.enumerator, eRender, eConsole, &perk_wasapi.device);
    kill_on_fail(hr, "Failed to retrieve DefaultAudioEndpoint!");

    hr = perk_wasapi.device->lpVtbl->Activate(perk_wasapi.device, &based_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&perk_wasapi.audio_client);
    kill_on_fail(hr, "Failed to activate device and create audio_client!");

    WAVEFORMATEXTENSIBLE *engine_format;

    hr = perk_wasapi.audio_client->lpVtbl->GetMixFormat(perk_wasapi.audio_client, (WAVEFORMATEX**)&engine_format);
    kill_on_fail(hr, "Failed to activate device and create audio_client!");

    WAVEFORMATEXTENSIBLE format = {
        .Format = {
            .wFormatTag      = WAVE_FORMAT_EXTENSIBLE,
            // TODO: Extract wanted channel count / preserving of original channel count into config.
            .nChannels       = 2,
            .nSamplesPerSec  = engine_format->Format.nSamplesPerSec,
            .nAvgBytesPerSec = engine_format->Format.nSamplesPerSec * 2 * sizeof(float),
            .nBlockAlign     = 2 * sizeof(float),
            .wBitsPerSample  = 8 * sizeof(float),
            .cbSize          = sizeof(WAVEFORMATEXTENSIBLE),
        },
        .Samples.wValidBitsPerSample = 8 * sizeof(float),
        .dwChannelMask = 0x3, // left, right; stereo
        .SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
    };

    CoTaskMemFree(engine_format);

    WAVEFORMATEXTENSIBLE *closest_format = NULL;

    hr = perk_wasapi.audio_client->lpVtbl->IsFormatSupported(perk_wasapi.audio_client, AUDCLNT_SHAREMODE_SHARED, &format.Format, (WAVEFORMATEX**)&closest_format);
    if (hr != S_OK) {
        if (hr == S_FALSE) {
            log("Requested wave format is not supported!");

            memcpy(&format, closest_format, sizeof(WAVEFORMATEXTENSIBLE));

            CoTaskMemFree(closest_format);
        }
        else {
            log("Call to is format supported failed!");
        }
    }

    // TODO: Extract into configuration.
    REFERENCE_TIME requested_duration = REFTIMES_PER_SEC / 8;

    hr = perk_wasapi.audio_client->lpVtbl->Initialize(perk_wasapi.audio_client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, requested_duration, 0, &format.Format, NULL);
    kill_on_fail(hr, "Failed to initialize audio_client!");

    unsigned buffer_frame_count;

    hr = perk_wasapi.audio_client->lpVtbl->GetBufferSize(perk_wasapi.audio_client, &buffer_frame_count);
    kill_on_fail(hr, "Failed to get buffer size ?!?!!");

    printf("application format:\n");
    perk_win32_print_waveformatextensible(&format);

    unsigned channel_count = format.Format.nChannels;

    static_format_info = (perk_format_info_t) {
        .sample_frequency = format.Format.nSamplesPerSec,
        .channel_count = channel_count,
    };

    // TODO: Memory barrier (ARM)?

    SetEvent(perk_wasapi.init_event);


    perk_wasapi.start_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (perk_wasapi.start_event == NULL) {
        log("Failed to create start_event!");
        exit(1);
    }

    WaitForSingleObject(perk_wasapi.start_event, INFINITE);

    perk_start_info_t info = static_start_info;

    CloseHandle(perk_wasapi.start_event);


    perk_wasapi.callback_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (perk_wasapi.callback_event == NULL) {
        log("Failed to create callback_event!");
        exit(1);
    }

    hr = perk_wasapi.audio_client->lpVtbl->SetEventHandle(perk_wasapi.audio_client, perk_wasapi.callback_event);
    kill_on_fail(hr, "Failed to set callback_event!\n");

    hr = perk_wasapi.audio_client->lpVtbl->GetService(perk_wasapi.audio_client, &based_IID_IAudioRenderClient, (void**)&perk_wasapi.render_client);
    kill_on_fail(hr, "Failed to retrieve render_client!");

    float *dest_buffer;

    hr = perk_wasapi.render_client->lpVtbl->GetBuffer(perk_wasapi.render_client, buffer_frame_count, (BYTE**)&dest_buffer);
    log_on_fail(hr, "Failed to get first buffer!");

    if (SUCCEEDED(hr)) {
        info.write_callback(dest_buffer, buffer_frame_count * channel_count, info.user_data);

        hr = perk_wasapi.render_client->lpVtbl->ReleaseBuffer(perk_wasapi.render_client, buffer_frame_count, 0);
        log_on_fail(hr, "Failed to release first buffer!");
    }

    hr = perk_wasapi.audio_client->lpVtbl->Start(perk_wasapi.audio_client);
    kill_on_fail(hr, "Failed to start audio client!\n");

    for (;;) {
        if (WaitForSingleObject(perk_wasapi.callback_event, 2000) != WAIT_OBJECT_0) {
            log("Possible timeout!\n");
        }

        unsigned padding_frames;

        hr = perk_wasapi.audio_client->lpVtbl->GetCurrentPadding(perk_wasapi.audio_client, &padding_frames);
        log_on_fail(hr, "Failed to retrieve padding!\n");

        unsigned frames_to_render = buffer_frame_count - padding_frames;

        if (SUCCEEDED(perk_wasapi.render_client->lpVtbl->GetBuffer(perk_wasapi.render_client, frames_to_render, (BYTE**)&dest_buffer))) {
            info.write_callback(dest_buffer, frames_to_render * channel_count, info.user_data);

            hr = perk_wasapi.render_client->lpVtbl->ReleaseBuffer(perk_wasapi.render_client, frames_to_render, 0);
            log_on_fail(hr, "Failed to release buffer!");
        }
    }
}


perk_format_info_t perk_init(void)
{
    perk_wasapi.init_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (perk_wasapi.init_event == NULL) {
        log("Failed to create init_event!");
        exit(1);
    }

    perk_wasapi.audio_thread = CreateThread(NULL, 0, perk_wasapi_audio_thread, NULL, 0, &perk_wasapi.audio_thread_id);

    if (!perk_wasapi.audio_thread) {
        log("Failed to create audio thread!\n");
    }

    WaitForSingleObject(perk_wasapi.init_event, INFINITE);

    CloseHandle(perk_wasapi.init_event);

    return static_format_info;
}


void perk_start(perk_start_info_t start_info)
{
    static_start_info = start_info;

    // TODO: Memory barrier (ARM)?

    SetEvent(perk_wasapi.start_event);
}


void perk_exit(void)
{
    perk_wasapi.audio_client->lpVtbl->Stop(perk_wasapi.audio_client);

    perk_wasapi.render_client->lpVtbl->Release(perk_wasapi.render_client);
    perk_wasapi.audio_client->lpVtbl->Release(perk_wasapi.audio_client);
    perk_wasapi.device->lpVtbl->Release(perk_wasapi.device);
    perk_wasapi.enumerator->lpVtbl->Release(perk_wasapi.enumerator);

    CloseHandle(perk_wasapi.callback_event);
    CloseHandle(perk_wasapi.audio_thread);

    CoUninitialize();
}
