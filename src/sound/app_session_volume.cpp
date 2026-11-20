// Windows keeps a volume / mute per application audio session and remembers it per .exe. The old waveOut
// backend (waveOutSetVolume = this app's session volume) left omsi_f4_viewer at 0 %, so every later run
// was silent whatever played. This walks every active output device, finds this process's sessions,
// logs their state and sets them to 100 % / unmuted.
#include "omsi/sound/app_session_volume.hpp"

#include "omsi/logger.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <string>

namespace omsi::sound {
namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

int restore_app_session_volume() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_owned = SUCCEEDED(co);
    int fixed = 0, seen = 0;
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* devs = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs))) {
        UINT count = 0;
        devs->GetCount(&count);
        const DWORD me = GetCurrentProcessId();
        for (UINT d = 0; d < count; ++d) {
            IMMDevice* dev = nullptr;
            IAudioSessionManager2* mgr = nullptr;
            IAudioSessionEnumerator* sessions = nullptr;
            IPropertyStore* props = nullptr;
            std::string dev_name = "?";
            if (FAILED(devs->Item(d, &dev))) continue;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) dev_name = narrow(v.pwszVal);
                PropVariantClear(&v);
            }
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) &&
                SUCCEEDED(mgr->GetSessionEnumerator(&sessions))) {
                int n = 0;
                sessions->GetCount(&n);
                for (int i = 0; i < n; ++i) {
                    IAudioSessionControl* ctl = nullptr;
                    IAudioSessionControl2* ctl2 = nullptr;
                    ISimpleAudioVolume* vol = nullptr;
                    if (SUCCEEDED(sessions->GetSession(i, &ctl)) &&
                        SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctl2)))) {
                        DWORD pid = 0;
                        ctl2->GetProcessId(&pid);
                        if (pid == me &&
                            SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&vol)))) {
                            ++seen;
                            float level = -1.f;
                            BOOL mute = FALSE;
                            vol->GetMasterVolume(&level);
                            vol->GetMute(&mute);
                            omsi::log::info("audio session on '" + dev_name + "': volume " + std::to_string(level) +
                                            (mute ? " MUTED" : ""));
                            if (level < 0.99f || mute) {
                                vol->SetMute(FALSE, nullptr);
                                vol->SetMasterVolume(1.f, nullptr);
                                ++fixed;
                                omsi::log::warn("audio session on '" + dev_name + "' restored to 100 % / unmuted");
                            }
                        }
                    }
                    release(vol);
                    release(ctl2);
                    release(ctl);
                }
            }
            release(sessions);
            release(mgr);
            release(props);
            release(dev);
        }
    }
    // waveOutSetVolume also set the session's *per-channel* volumes (left / right words of its DWORD):
    // a session at master 100 % with both channels at 0 is still silent. The channel volumes are only
    // reachable through an IAudioClient of this process's default session on each device.
    if (devs) {
        UINT count = 0;
        devs->GetCount(&count);
        for (UINT d = 0; d < count; ++d) {
            IMMDevice* dev = nullptr;
            IAudioClient* client = nullptr;
            IChannelAudioVolume* chans = nullptr;
            ISimpleAudioVolume* simple = nullptr;
            WAVEFORMATEX* fmt = nullptr;
            if (FAILED(devs->Item(d, &dev))) continue;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
                SUCCEEDED(client->GetMixFormat(&fmt)) &&
                SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt, nullptr))) {
                if (SUCCEEDED(client->GetService(__uuidof(IChannelAudioVolume), reinterpret_cast<void**>(&chans)))) {
                    UINT nch = 0;
                    chans->GetChannelCount(&nch);
                    std::string levels;
                    bool low = false;
                    for (UINT c = 0; c < nch; ++c) {
                        float v = 1.f;
                        chans->GetChannelVolume(c, &v);
                        levels += (c ? "/" : "") + std::to_string(v).substr(0, 4);
                        if (v < 0.99f) low = true;
                    }
                    if (low) {
                        for (UINT c = 0; c < nch; ++c) chans->SetChannelVolume(c, 1.f, nullptr);
                        ++fixed;
                        omsi::log::warn("audio session channel volumes were " + levels + " on device " + std::to_string(d) +
                                        " - restored to 100 %");
                    } else {
                        omsi::log::info("audio session channel volumes " + levels + " on device " + std::to_string(d));
                    }
                }
                if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&simple)))) {
                    simple->SetMute(FALSE, nullptr);
                    float lv = 1.f;
                    simple->GetMasterVolume(&lv);
                    if (lv < 0.99f) simple->SetMasterVolume(1.f, nullptr);
                }
            }
            if (fmt) CoTaskMemFree(fmt);
            release(simple);
            release(chans);
            release(client);
            release(dev);
        }
    }
    release(devs);
    release(en);
    if (seen == 0) omsi::log::info("audio session: this process has no audio session yet");
    if (co_owned) CoUninitialize();
    return fixed;
}

} // namespace omsi::sound
#else
namespace omsi::sound {
int restore_app_session_volume() { return 0; }
} // namespace omsi::sound
#endif
