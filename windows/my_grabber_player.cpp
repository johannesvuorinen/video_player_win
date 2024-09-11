// ref: https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/medfound/using-the-sample-grabber-sink.md
// ref: https://learn.microsoft.com/en-us/windows/win32/medfound/seeking--fast-forward--and-reverse-play
// ref: https://blog.csdn.net/u013113678/article/details/125492286
// ref: https://blog.csdn.net/weixin_40256196/article/details/127021206

#include "my_grabber_player.h"

#include <Shlwapi.h>
//#include <mfapi.h>
//#include <mfidl.h>
#include <mfreadwrite.h>
#include <new>
#include <iostream>

#include <mmdeviceapi.h>
#include <audiopolicy.h>

#pragma comment(lib, "mf")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "Shlwapi")
#pragma comment(lib, "Mfreadwrite")

#define CHECK_HR(x) if (FAILED(x)) { goto done; }

class CAsyncCallback : public IMFAsyncCallback
{
    // ref: https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/DX11VideoRenderer/cpp/Common.h
public:

    typedef std::function<HRESULT(IMFAsyncResult*)> InvokeFn;
    //typedef HRESULT(InvokeFn)(IMFAsyncResult* pAsyncResult);

    CAsyncCallback(InvokeFn fn) :
        m_pInvokeFn(fn)
    {
    }

    // IUnknown
    inline STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_cRef);
    }
    STDMETHODIMP_(ULONG) Release() {
        ULONG uCount = InterlockedDecrement(&m_cRef);
        if (uCount == 0) delete this;
        return uCount;
    }

    STDMETHODIMP QueryInterface(REFIID iid, __RPC__deref_out _Result_nullonfailure_ void** ppv)
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown))
        {
            *ppv = static_cast<IUnknown*>(static_cast<IMFAsyncCallback*>(this));
        }
        else if (iid == __uuidof(IMFAsyncCallback))
        {
            *ppv = static_cast<IMFAsyncCallback*>(this);
        }
        else
        {
            *ppv = NULL;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    // IMFAsyncCallback methods
    STDMETHODIMP GetParameters(__RPC__out DWORD* pdwFlags, __RPC__out DWORD* pdwQueue)
    {
        // Implementation of this method is optional.
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(__RPC__in_opt IMFAsyncResult* pAsyncResult)
    {
        return (m_pInvokeFn)(pAsyncResult);
    }

private:

    long m_cRef = 1;
    InvokeFn m_pInvokeFn;
};

// --------------------------------------------------------------------------

class SampleGrabberCB : public IMFSampleGrabberSinkCallback
{
    long m_cRef;
    wil::com_ptr<MyPlayerCallback> m_pUserCallback;

    SampleGrabberCB() : m_cRef(1) {}

public:
    static HRESULT CreateInstance(SampleGrabberCB** ppCB);

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    void SetUserCallback(MyPlayerCallback* cb) { m_pUserCallback = cb; } //Jacky

    // IMFClockStateSink methods
    STDMETHODIMP OnClockStart(MFTIME hnsSystemTime, LONGLONG llClockStartOffset);
    STDMETHODIMP OnClockStop(MFTIME hnsSystemTime);
    STDMETHODIMP OnClockPause(MFTIME hnsSystemTime);
    STDMETHODIMP OnClockRestart(MFTIME hnsSystemTime);
    STDMETHODIMP OnClockSetRate(MFTIME hnsSystemTime, float flRate);

    // IMFSampleGrabberSinkCallback methods
    STDMETHODIMP OnSetPresentationClock(IMFPresentationClock* pClock);
    STDMETHODIMP OnProcessSample(REFGUID guidMajorMediaType, DWORD dwSampleFlags,
        LONGLONG llSampleTime, LONGLONG llSampleDuration, const BYTE* pSampleBuffer,
        DWORD dwSampleSize);
    STDMETHODIMP OnShutdown();
};

HRESULT CreateTopology(IMFMediaSource* pSource, IMFActivate* pSink, IMFTopology** ppTopo);

// --------------------------------------------------------------------------

MyPlayer::MyPlayer() :
    m_hnsDuration(-1),
    m_VideoWidth(0),
    m_VideoHeight(0),
    m_isShutdown(false)
{
    initAudioVolume();
}

MyPlayer::~MyPlayer()
{
    Shutdown();
    std::cout << "[native] ~MyPlayer()" << std::endl;
}

HRESULT MyPlayer::initAudioVolume()
{
    HRESULT hr = S_OK;

    wil::com_ptr<IMMDeviceEnumerator> pDeviceEnumerator;
    wil::com_ptr<IMMDevice> pDevice;
    wil::com_ptr<IAudioSessionManager> pAudioSessionManager;

    // Get the enumerator for the audio endpoint devices.
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pDeviceEnumerator)
    );

    if (FAILED(hr)) { goto done; }

    // Get the default audio endpoint that the SAR will use.
    hr = pDeviceEnumerator->GetDefaultAudioEndpoint(
        eRender,
        eConsole,   // The SAR uses 'eConsole' by default.
        &pDevice
    );
    if (FAILED(hr)) { goto done; }

    // Get the session manager for this device.
    hr = pDevice->Activate(
        __uuidof(IAudioSessionManager),
        CLSCTX_INPROC_SERVER,
        NULL,
        (void**)&pAudioSessionManager
    );
    if (FAILED(hr)) { goto done; }

    hr = pAudioSessionManager->GetSimpleAudioVolume(
        &GUID_NULL, 0, &m_pSimpleAudioVolume
    );

done:
    return hr;
}

HRESULT MyPlayer::OpenURL(const WCHAR* pszFileName, MyPlayerCallback* playerCallback, HWND hwndVideo, std::function<void(bool)> loadCallback)
{
    this->AddRef(); // keep *this alive before callback called
    HRESULT hr = CreateMediaSourceAsync(pszFileName, [=](IMFMediaSource* pSource) -> void {
        HRESULT hr;
        wil::com_ptr<SampleGrabberCB> pCallback;
        wil::com_ptr<IMFTopology> pTopology;
        wil::com_ptr<IMFMediaType> pType;
        wil::com_ptr<IMFClock> pClock;

        if (this->Release() <= 0 || pSource == NULL) {
            //load fail or abort
            if (pSource) pSource->Release();
            loadCallback(false);
            return; // *this* maybe already deleted, so don't access any *this members, and return immediately!
        }
        m_pMediaSource = pSource;
        pSource = NULL;

        // Configure the media type that the Sample Grabber will receive.
        // Setting the major and subtype is usually enough for the topology loader
        // to resolve the topology.

        CHECK_HR(hr = MFCreateMediaType(&pType));
        CHECK_HR(hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        CHECK_HR(hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)); //OK
        //CHECK_HR(hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32)); //fail

        if (playerCallback != NULL) //Jacky
        {
            // Create the sample grabber sink.
            CHECK_HR(hr = SampleGrabberCB::CreateInstance(&pCallback));
            pCallback->SetUserCallback(playerCallback);
            CHECK_HR(hr = MFCreateSampleGrabberSinkActivate(pType.get(), pCallback.get(), &m_pVideoSinkActivate)); //Jacky
        }
        else
        {
            CHECK_HR(hr = MFCreateVideoRendererActivate(hwndVideo, &m_pVideoSinkActivate)); //Jacky
        }

        // To run as fast as possible, set this attribute (requires Windows 7):
        CHECK_HR(hr = m_pVideoSinkActivate->SetUINT32(MF_SAMPLEGRABBERSINK_IGNORE_CLOCK, FALSE)); //Jacky

        // Create the Media Session.
        CHECK_HR(hr = MFCreateMediaSession(NULL, &m_pSession));

        // Create the topology.
        CHECK_HR(hr = CreateTopology(m_pMediaSource.get(), m_pVideoSinkActivate.get(), &pTopology));

        // Run the media session.
        CHECK_HR(hr = m_pSession->SetTopology(0, pTopology.get()));

        // Get the presentation clock (optional)
        CHECK_HR(hr = m_pSession->GetClock(&pClock));
        CHECK_HR(hr = pClock->QueryInterface(IID_PPV_ARGS(&m_pClock)));

        // Get the rate control interface (optional)
        CHECK_HR(MFGetService(m_pSession.get(), MF_RATE_CONTROL_SERVICE, IID_PPV_ARGS(&m_pRate)));

        // add event listener
        m_pSession->BeginGetEvent(this, NULL);

        /* Jacky test, try to get volume control fail... {
        {
            UINT32 channelsCount;
            float volumes[100];
            IMFAudioStreamVolume* pAudioVolume = NULL;
            CHECK_HR(hr = MFGetService(m_pSession, MR_STREAM_VOLUME_SERVICE, IID_PPV_ARGS(&pAudioVolume))); // will fail here... no such interface error...
            CHECK_HR(hr = pAudioVolume->GetChannelCount(&channelsCount));
            for (int i = 0; i < channelsCount; i++) volumes[i] = 10;
            CHECK_HR(hr = pAudioVolume->SetAllVolumes(channelsCount, volumes));
        }
        */
        if (m_isShutdown) hr = E_FAIL;

    done:
        // Clean up.
        if (FAILED(hr)) Shutdown();
        loadCallback(SUCCEEDED(hr));
        });

    // Clean up.
    if (FAILED(hr)) Shutdown();
    return hr;
}

HRESULT MyPlayer::Play(LONGLONG ms)
{
    if (m_pSession == NULL) return E_FAIL;
    if (ms >= 0) return Seek(ms);

    PROPVARIANT var;
    PropVariantInit(&var);
    return m_pSession->Start(NULL, &var);
}

HRESULT MyPlayer::Pause()
{
    if (m_pSession == NULL) return E_FAIL;
    return m_pSession->Pause();
}

LONGLONG MyPlayer::GetDuration()
{
    if (m_pSession == NULL) return -1;
    return m_hnsDuration / 10000;
}

LONGLONG MyPlayer::GetCurrentPosition()
{
    MFTIME pos;
    HRESULT hr;
    if (m_pSession == NULL) return -1;
    hr = m_pClock->GetTime(&pos);
    if (FAILED(hr)) return -1;
    return pos / 10000;
}

HRESULT MyPlayer::Seek(LONGLONG ms)
{
    PROPVARIANT var;
    if (m_pSession == NULL) return E_FAIL;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = ms * 10000;
    return m_pSession->Start(NULL, &var);
}

SIZE MyPlayer::GetVideoSize()
{
    SIZE size;
    size.cx = m_VideoWidth;
    size.cy = m_VideoHeight;
    return size;
}

HRESULT MyPlayer::SetPlaybackSpeed(float speed)
{
    if (m_pSession == NULL) return E_FAIL;
    return m_pRate->SetRate(FALSE, speed);
}

HRESULT MyPlayer::GetVolume(float* pVol)
{
    if (m_pSession == NULL) return E_FAIL;
    return m_pSimpleAudioVolume->GetMasterVolume(pVol);
}

HRESULT MyPlayer::SetVolume(float vol)
{
    if (m_pSession == NULL) return E_FAIL;
    return m_pSimpleAudioVolume->SetMasterVolume(vol, NULL);
}

HRESULT MyPlayer::SetMute(bool bMute)
{
    if (m_pSession == NULL) return E_FAIL;
    return m_pSimpleAudioVolume->SetMute(bMute, NULL);
}


void MyPlayer::Shutdown()
{
    std::unique_lock<std::mutex> guard(m_mutex);
    if (m_isShutdown) return;
    m_isShutdown = true;
    m_hnsDuration = -1;
    cancelAsyncLoad();

    // NOTE: because m_pSession->BeginGetEvent(this) will keep *this,
    //       so we need to call m_pSession->Shutdown() first
    //       then client call player->Release() will make refCount = 0
    if (m_pSession) {
        m_pSession->Stop();
        m_pSession->Close();

        m_pSession->Shutdown();
        m_pMediaSource->Shutdown();
        if (m_pVideoSinkActivate.get() != NULL) m_pVideoSinkActivate->ShutdownObject();
        if (m_pAudioRendererActivate.get() != NULL) m_pAudioRendererActivate->ShutdownObject();
    }
}

void MyPlayer::cancelAsyncLoad() {
    if (m_pSourceResolver != NULL && m_pSourceResolverCancelCookie != NULL) {
        m_pSourceResolver->CancelObjectCreation(m_pSourceResolverCancelCookie.get());
        m_pSourceResolver.reset();
        m_pSourceResolverCancelCookie.reset();
    }
}

HRESULT MyPlayer::GetParameters(DWORD* pdwFlags, DWORD* pdwQueue)
{
    return S_OK;
}

HRESULT MyPlayer::Invoke(IMFAsyncResult* pResult)
{
    std::unique_lock<std::mutex> guard(m_mutex);
    wil::com_ptr<MyPlayer> thisRef(this);
    HRESULT hr;
    wil::com_ptr<IMFMediaEvent> pEvent;
    MediaEventType meType = MEUnknown;

    if (m_isShutdown || m_pSession == NULL) return E_FAIL;
    CHECK_HR(hr = m_pSession->EndGetEvent(pResult, &pEvent));
    CHECK_HR(hr = pEvent->GetType(&meType));
    CHECK_HR(hr = m_pSession->BeginGetEvent(this, NULL));

    //std::cout << "native player event: " << meType << std::endl;
    switch (meType) {
    case MESessionStarted:
    case MEBufferingStarted:
    case MEBufferingStopped:
    case MESessionPaused:
    case MESessionStopped:
    case MESessionClosed:
    case MESessionEnded:
    case MEError:
        OnPlayerEvent(meType);
        break;
    }

done:
    return S_OK;
}

// --------------------------------------------------------------------------

// Create a media source from a URL.
HRESULT MyPlayer::CreateMediaSourceAsync(PCWSTR pszURL, std::function<void(IMFMediaSource* pSource)> callback)
{
    // Create the source resolver.
    HRESULT hr = S_OK;
    CAsyncCallback* cb = NULL;
    CHECK_HR(hr = MFCreateSourceResolver(&m_pSourceResolver));

    this->AddRef(); // prevent *this released before callback
    hr = m_pSourceResolver->BeginCreateObjectFromURL(pszURL,
        MF_RESOLUTION_MEDIASOURCE, NULL, &m_pSourceResolverCancelCookie,
        cb = new CAsyncCallback([=](IMFAsyncResult* pResult) -> HRESULT {
            HRESULT hr;
            MF_OBJECT_TYPE ObjectType;
            wil::com_ptr<IUnknown> pSource;
            wil::com_ptr<IMFMediaSource> pMediaSource;

            if (this->Release() <= 0 || m_isShutdown) {
                //pResult->Release();
                callback(NULL);
                return E_FAIL; // *this* maybe already deleted, so don't access any *this members, and return immediately!
            }

            if (m_pSourceResolver) { // m_pSourceResolver maybe null since Shutdown() called immediately after OpenURL()
                CHECK_HR(hr = m_pSourceResolver->EndCreateObjectFromURL(pResult, &ObjectType, &pSource));
                CHECK_HR(hr = pSource->QueryInterface(IID_PPV_ARGS(&pMediaSource)));
            }
            else
            {
                hr = E_FAIL;
                goto done;
            }

            pMediaSource->AddRef();
            callback(pMediaSource.get());

        done:
            //m_pSourceResolver.reset();
            //m_pSourceResolverCancelCookie.reset();
            if (FAILED(hr)) callback(NULL);
            return S_OK;
            }),
        NULL);
    CHECK_HR(hr);

done:
    if (cb) cb->Release();
    if (FAILED(hr)) {
        m_pSourceResolver.reset();
        m_pSourceResolverCancelCookie.reset();
    }
    return hr;
}


// Add a source node to a topology.
HRESULT AddSourceNode(
    IMFTopology* pTopology,           // Topology.
    IMFMediaSource* pSource,          // Media source.
    IMFPresentationDescriptor* pPD,   // Presentation descriptor.
    IMFStreamDescriptor* pSD,         // Stream descriptor.
    IMFTopologyNode** ppNode)         // Receives the node pointer.
{
    wil::com_ptr<IMFTopologyNode> pNode;

    HRESULT hr = S_OK;
    CHECK_HR(hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pNode));
    CHECK_HR(hr = pNode->SetUnknown(MF_TOPONODE_SOURCE, pSource));
    CHECK_HR(hr = pNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD));
    CHECK_HR(hr = pNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD));
    CHECK_HR(hr = pTopology->AddNode(pNode.get()));

    // Return the pointer to the caller.
    *ppNode = pNode.get();
    (*ppNode)->AddRef();

done:
    return hr;
}

// Add an output node to a topology.
HRESULT AddOutputNode(
    IMFTopology* pTopology,     // Topology.
    IMFActivate* pActivate,     // Media sink activation object.
    DWORD dwId,                 // Identifier of the stream sink.
    IMFTopologyNode** ppNode)   // Receives the node pointer.
{
    wil::com_ptr<IMFTopologyNode> pNode;

    HRESULT hr = S_OK;
    CHECK_HR(hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pNode));
    CHECK_HR(hr = pNode->SetObject(pActivate));
    //CHECK_HR(hr = pNode->SetUINT32(MF_TOPONODE_STREAMID, dwId));
    //CHECK_HR(hr = pNode->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE, FALSE));
    CHECK_HR(hr = pTopology->AddNode(pNode.get()));

    // Return the pointer to the caller.
    *ppNode = pNode.get();
    (*ppNode)->AddRef();

done:
    return hr;
}

// Create the topology.
HRESULT MyPlayer::CreateTopology(IMFMediaSource* pSource, IMFActivate* pSinkActivate, IMFTopology** ppTopo)
{
    wil::com_ptr<IMFTopology> pTopology;
    wil::com_ptr<IMFPresentationDescriptor> pPD;
    wil::com_ptr<IMFStreamDescriptor> pSD;
    wil::com_ptr<IMFMediaTypeHandler> pHandler;
    wil::com_ptr<IMFTopologyNode> pNodeSrc; // source node
    wil::com_ptr<IMFTopologyNode> pNodeVideoSink; // video node
    wil::com_ptr<IMFTopologyNode> pNodeAudioSink; // audio node
    wil::com_ptr<IMFMediaType> pVideoMediaType;
    bool isSourceAdded = false;

    HRESULT hr = S_OK;
    DWORD cStreams = 0;

    m_VideoWidth = m_VideoHeight = 0;

    CHECK_HR(hr = MFCreateTopology(&pTopology));
    CHECK_HR(hr = pSource->CreatePresentationDescriptor(&pPD));
    CHECK_HR(hr = pPD->GetStreamDescriptorCount(&cStreams));

    for (DWORD i = 0; i < cStreams; i++)
    {
        // In this example, we look for audio streams and connect them to the sink.

        BOOL fSelected = FALSE;
        GUID majorType;

        CHECK_HR(hr = pPD->GetStreamDescriptorByIndex(i, &fSelected, &pSD));
        CHECK_HR(hr = pSD->GetMediaTypeHandler(&pHandler));
        CHECK_HR(hr = pHandler->GetMajorType(&majorType));

        if (majorType == MFMediaType_Video && fSelected) //Jacky
        {
            if (!isSourceAdded)
            {
                CHECK_HR(hr = AddSourceNode(pTopology.get(), pSource, pPD.get(), pSD.get(), &pNodeSrc));
                isSourceAdded = true;
            }

            CHECK_HR(hr = AddSourceNode(pTopology.get(), pSource, pPD.get(), pSD.get(), &pNodeSrc));
            CHECK_HR(hr = AddOutputNode(pTopology.get(), pSinkActivate, 0, &pNodeVideoSink));
            CHECK_HR(hr = pNodeSrc->ConnectOutput(0, pNodeVideoSink.get(), 0));

            // get video resolution
            CHECK_HR(hr = pHandler->GetCurrentMediaType(&pVideoMediaType));
            MFGetAttributeSize(pVideoMediaType.get(), MF_MT_FRAME_SIZE, &m_VideoWidth, &m_VideoHeight);
        }
        else if (majorType == MFMediaType_Audio && fSelected)
        {
            //Jacky
            if (!isSourceAdded)
            {
                CHECK_HR(hr = AddSourceNode(pTopology.get(), pSource, pPD.get(), pSD.get(), &pNodeSrc));
                isSourceAdded = true;
            }
            CHECK_HR(hr = MFCreateAudioRendererActivate(&m_pAudioRendererActivate));
            CHECK_HR(hr = AddOutputNode(pTopology.get(), m_pAudioRendererActivate.get(), 0, &pNodeAudioSink));
            CHECK_HR(hr = pNodeSrc->ConnectOutput(0, pNodeAudioSink.get(), 0));

            /* Jacky test, try to get volume control fail... {
            {
                UINT32 channelsCount;
                float volumes[100];
                IMFAudioStreamVolume* pAudioVolume = NULL;
                CHECK_HR(hr = MFGetService(pRendererActivate, MR_STREAM_VOLUME_SERVICE, IID_PPV_ARGS(&pAudioVolume))); // will fail here... no such interface error...
                CHECK_HR(hr = pAudioVolume->GetChannelCount(&channelsCount));
                for (int i = 0; i < channelsCount; i++) volumes[i] = 10;
                CHECK_HR(hr = pAudioVolume->SetAllVolumes(channelsCount, volumes));
            }
            */
        }
        else
        {
            CHECK_HR(hr = pPD->DeselectStream(i));
        }
    }

    CHECK_HR(pPD->GetUINT64(MF_PD_DURATION, (UINT64*)&m_hnsDuration));

    *ppTopo = pTopology.get();
    (*ppTopo)->AddRef();

done:
    return hr;
}

// SampleGrabberCB implementation

// Create a new instance of the object.
HRESULT SampleGrabberCB::CreateInstance(SampleGrabberCB** ppCB)
{
    *ppCB = new (std::nothrow) SampleGrabberCB();

    if (ppCB == NULL)
    {
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

STDMETHODIMP SampleGrabberCB::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(SampleGrabberCB, IMFSampleGrabberSinkCallback),
        QITABENT(SampleGrabberCB, IMFClockStateSink),
        { 0 }
    };
    return QISearch(this, qit, riid, ppv);
}

STDMETHODIMP_(ULONG) SampleGrabberCB::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG) SampleGrabberCB::Release()
{
    ULONG cRef = InterlockedDecrement(&m_cRef);
    if (cRef == 0)
    {
        delete this;
    }
    return cRef;

}

// IMFClockStateSink methods.

// In these example, the IMFClockStateSink methods do not perform any actions.
// You can use these methods to track the state of the sample grabber sink.

STDMETHODIMP SampleGrabberCB::OnClockStart(MFTIME hnsSystemTime, LONGLONG llClockStartOffset)
{
    return S_OK;
}

STDMETHODIMP SampleGrabberCB::OnClockStop(MFTIME hnsSystemTime)
{
    return S_OK;
}

STDMETHODIMP SampleGrabberCB::OnClockPause(MFTIME hnsSystemTime)
{
    return S_OK;
}

STDMETHODIMP SampleGrabberCB::OnClockRestart(MFTIME hnsSystemTime)
{
    return S_OK;
}

STDMETHODIMP SampleGrabberCB::OnClockSetRate(MFTIME hnsSystemTime, float flRate)
{
    return S_OK;
}

// IMFSampleGrabberSink methods.

STDMETHODIMP SampleGrabberCB::OnSetPresentationClock(IMFPresentationClock* pClock)
{
    return S_OK;
}

STDMETHODIMP SampleGrabberCB::OnProcessSample(REFGUID guidMajorMediaType, DWORD dwSampleFlags,
    LONGLONG llSampleTime, LONGLONG llSampleDuration, const BYTE* pSampleBuffer,
    DWORD dwSampleSize)
{
    if (m_pUserCallback.get() == NULL) return S_OK;
    m_pUserCallback->OnProcessSample(guidMajorMediaType, dwSampleFlags,
        llSampleTime, llSampleDuration, pSampleBuffer,
        dwSampleSize);
    return S_OK;
}

STDMETHODIMP SampleGrabberCB::OnShutdown()
{
    m_pUserCallback.reset();
    return S_OK;
}
