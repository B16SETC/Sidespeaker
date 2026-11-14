#include "capture.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <cstdio>

#define TRY(x) do { HRESULT h_ = (x); if (FAILED(h_)) { char b_[96]; snprintf(b_, sizeof b_, "%s (0x%08lx)", #x, (unsigned long)h_); err = b_; return false; } } while (0)

namespace {

class Wasapi : public Capture {
public:
    ~Wasapi() override {
        if (ac_) ac_->Stop();
        if (cap_) cap_->Release();
        if (ac_) ac_->Release();
        if (dev_) dev_->Release();
        if (en_) en_->Release();
        if (wf_) CoTaskMemFree(wf_);
        if (com_) CoUninitialize();
    }

    bool open(std::string& err) override {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        TRY(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en_));
        TRY(en_->GetDefaultAudioEndpoint(eRender, eConsole, &dev_));
        TRY(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac_));
        TRY(ac_->GetMixFormat(&wf_));
        TRY(ac_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, wf_, nullptr));
        TRY(ac_->GetService(__uuidof(IAudioCaptureClient), (void**)&cap_));
        rate_ = wf_->nSamplesPerSec;
        ch_ = wf_->nChannels;
        outCh_ = std::min<int>(ch_, 2);
        isFloat_ = wf_->wBitsPerSample == 32;
        return true;
    }

    uint32_t sampleRate() const override { return rate_; }
    int channels() const override { return outCh_; }

    bool start() override {
        ac_->Stop();
        ac_->Reset();
        return SUCCEEDED(ac_->Start());
    }

    void stop() override { ac_->Stop(); }

    int read(std::vector<int16_t>& out) override {
        out.clear();
        UINT32 n = 0;
        if (FAILED(cap_->GetNextPacketSize(&n))) return -1;
        int total = 0;
        while (n > 0) {
            BYTE* d = nullptr;
            UINT32 fr = 0;
            DWORD fl = 0;
            if (FAILED(cap_->GetBuffer(&d, &fr, &fl, nullptr, nullptr))) return -1;
            size_t base = out.size();
            out.resize(base + (size_t)fr * outCh_, 0);
            if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
                for (UINT32 i = 0; i < fr; i++) {
                    for (int c = 0; c < outCh_; c++) {
                        if (isFloat_) {
                            float v = ((const float*)d)[i * ch_ + c];
                            v = std::max(-1.0f, std::min(1.0f, v));
                            out[base + (size_t)i * outCh_ + c] = (int16_t)(v * 32767.0f);
                        } else {
                            out[base + (size_t)i * outCh_ + c] = ((const int16_t*)d)[i * ch_ + c];
                        }
                    }
                }
            }
            cap_->ReleaseBuffer(fr);
            total += (int)fr;
            if (FAILED(cap_->GetNextPacketSize(&n))) return -1;
        }
        return total;
    }

private:
    IMMDeviceEnumerator* en_ = nullptr;
    IMMDevice* dev_ = nullptr;
    IAudioClient* ac_ = nullptr;
    IAudioCaptureClient* cap_ = nullptr;
    WAVEFORMATEX* wf_ = nullptr;
    uint32_t rate_ = 0;
    int ch_ = 0;
    int outCh_ = 0;
    bool isFloat_ = false;
    bool com_ = false;
};

}

std::unique_ptr<Capture> makeCapture() {
    return std::make_unique<Wasapi>();
}
