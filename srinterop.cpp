// dllmain.cpp : Defines the entry point for the DLL application.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
}

#include <d3d11.h>
#include <dxgi1_2.h>
#include <libavcodec/d3d11va.h>
#include <libavutil/hwcontext_d3d11va.h>

#define INTEL_ALIGN16(value)                      (((value + 15) >> 4) << 4)

static ID3D11Device*             render_device  = nullptr;
static ID3D11DeviceContext*  render_device_ctx  = nullptr;
static ID3D11VideoDevice*  render_video_device  = nullptr;
static ID3D11VideoContext* render_video_context = nullptr;
static ID3D11Texture2D*    srOutputTexture      = nullptr;
static ID3D11Texture2D*    srOutputStaging      = nullptr;
static bool bVideoProcessorInitialized          = false;

ID3D11VideoProcessorEnumerator* video_processors_enum_ = nullptr;
ID3D11VideoProcessor*                 video_processor_ = nullptr;
ID3D11VideoProcessorInputView*             input_view_ = nullptr;
ID3D11VideoProcessorOutputView*           output_view_ = nullptr;
FILE* pOutputFile = nullptr;
FILE* pLogFile= nullptr;
static unsigned int renderIndex = 0;
typedef struct _VPE_FUNCTION
{
    UINT                                        Function;               // [pInputFile]
    union                                                               // [pInputFile]
    {
        void* pSrCalingmode;
        void* pVPEMode;
        void* pVPEVersion;
        void* pSRParams;
    };
} VPE_FUNCTION, * PVPE_FUNCTION;
typedef enum _VPE_SUPER_RESOLUTION_MODE
{
    DEFAULT_SCENARIO_MODE = 0,
    CAMERA_SCENARIO_MODE = 1
}VPE_SUPER_RESOLUTION_MODE;
typedef struct _VPE_SR_PARAMS
{
    UINT                                         bEnable : 1;   // [in], Enable SR
    UINT                                         ReservedBits : 31;
    VPE_SUPER_RESOLUTION_MODE                    SRMode;        // [in], SRMode is one of VPE_SUPER_RESOLUTION_MODE
    UINT                                         Reserved[4];
}VPE_SR_PARAMS, * PVPE_SR_PARAMS;

typedef struct _SR_SCALING_MODE
{
    UINT Fastscaling;
    // to be extention
    // where customer can pass the training model data to DXVA driver
}SR_SCALING_MODE, * PSR_SCALING_MODE;

typedef struct _VPE_VERSION
{
    UINT Version;
}VPE_VERSION, * PVPE_VERSION;

enum VPEMode
{
    VPE_MODE_NONE = 0x0,
    VPE_MODE_PREPROC = 0x1,
};

enum VPE_VERSION_ENUM
{
    VPE_VERSION_1_0 = 0x0001,
    VPE_VERSION_2_0 = 0x0002,
    VPE_VERSION_3_0 = 0x0003,   // CNL new campipe interface
    VPE_VERSION_UNKNOWN = 0xffff,
};
typedef struct _VPE_MODE
{
    UINT Mode;
}VPE_MODE, * PVPE_MODE;

enum ScalingMode
{
    SCALING_MODE_DEFAULT = 0,                 // Default
    SCALING_MODE_QUALITY,                     // LowerPower
    SCALING_MODE_SUPERRESOLUTION              // SuperREsolution
};

#define VPE_FN_SCALING_MODE_PARAM            0x37
#define VPE_FN_MODE_PARAM                    0x20
#define VPE_FN_SET_VERSION_PARAM             0x01
#define VPE_FN_SR_SET_PARAMS                 0x401

static const GUID GUID_VPE_INTERFACE =
{ 0xedd1d4b9, 0x8659, 0x4cbc,{ 0xa4, 0xd6, 0x98, 0x31, 0xa2, 0x16, 0x3a, 0xc3 } };

unsigned char* out[4] = { NULL };
static unsigned int ncurrIndex = 0;
extern "C" __declspec(dllexport) int SuperResolution(AVFrame * avframe, AVFrame * outavframe , byte * out, int width, int height, int verbose);

void resetContext()
{
    if (output_view_) {
        output_view_->Release();
        output_view_ = nullptr;
    }
    if (input_view_) {
        input_view_->Release();
        input_view_ = nullptr;
    }
    if (video_processors_enum_) {
        video_processors_enum_->Release();
        video_processors_enum_ = nullptr;
    }
    if (video_processor_) {
        video_processor_->Release();
        video_processor_ = nullptr;
    }

    if (srOutputTexture) {
        srOutputTexture->Release();
        srOutputTexture = nullptr;
    }

    if (srOutputStaging) {
        srOutputStaging->Release();
        srOutputStaging = nullptr;
    }

    if (render_device_ctx) {
        render_device_ctx->Release();
        render_device_ctx = nullptr;
    }

    if (render_video_context) {
        render_video_context->Release();
        render_video_context = nullptr;
    }

    if (render_video_device) {
        render_video_device->Release();
        render_video_device = nullptr;
    }
    if (render_device) {
        render_device->Release();
        render_device = nullptr;
    }

    for (int nindex = 0; nindex < 4; nindex++) {
        free(out[nindex]);
    }

    bVideoProcessorInitialized = false;
}
bool InitContext(ID3D11Texture2D *texture, int inWidth, int inHeight, int width, int height)
{
    HRESULT hr = S_OK;
    char szBuffer[128] = { 0 };
    if (false == bVideoProcessorInitialized)
    {
        if (!render_device) {
            texture->GetDevice(&render_device);
            render_device->GetImmediateContext(&render_device_ctx);
        }
        if (!render_video_device) {
            hr = render_device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&render_video_device);
            if (FAILED(hr)) {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Failed to Query D3D11 Device from AVFrame \r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
                goto fail;
            }
            else {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Get D3D11 Device from AVFrame  Success\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
            }
        }
        if (!render_video_context) {
            hr = render_device_ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&render_video_context);
            if (FAILED(hr)) {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Failed to get D3D11 Video device context  from AVFrame  Success\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
                goto fail;
            }
            else {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Gett D3D11 Video device context  from AVFrame  Success\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
            }
        }

        if (hr == S_OK && bVideoProcessorInitialized == false)
        {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc;
            memset(&content_desc, 0, sizeof(content_desc));

            // Non-scaling. If you're going to do SR here, update the desc here.
            content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content_desc.InputFrameRate.Numerator = 60;
            content_desc.InputFrameRate.Denominator = 1;
            content_desc.InputWidth   = inWidth;
            content_desc.InputHeight  = inHeight;
            content_desc.OutputWidth  = width;
            content_desc.OutputHeight = height;
            content_desc.OutputFrameRate.Numerator = 60;
            content_desc.OutputFrameRate.Denominator = 1;
            content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            HRESULT hr = render_video_device->CreateVideoProcessorEnumerator(
                &content_desc, &video_processors_enum_);
           
            if (SUCCEEDED(hr)) {
                if (video_processor_)
                    video_processor_->Release();
                hr = render_video_device->CreateVideoProcessor(video_processors_enum_,
                    0, &video_processor_);

                if (FAILED(hr)) {
                    if (nullptr != pLogFile) {
                        memset(szBuffer, 0, 128);
                        sprintf_s(szBuffer, "failed to create video processor\r");
                        fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                    }
                    goto fail;
                }
                else {
                    if (nullptr != pLogFile) {
                        memset(szBuffer, 0, 128);
                        sprintf_s(szBuffer, "Create Video Processor Success\r");
                        fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                    }
                }
                bVideoProcessorInitialized = true;
            }
            else {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "failed to create video processor\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
                goto fail;
            }

            for (int nindex = 0; nindex < 4; nindex++) {
                out[nindex] = (unsigned char*)malloc(width * INTEL_ALIGN16(height) * 3 / 2);
            }
        }
        bVideoProcessorInitialized = true;
    }
    return true;
fail:
    resetContext();
    return false;
}
int SuperResolution(AVFrame* avframe, AVFrame* outavframe, byte* out2, int width, int height, int verbose)
{
    HRESULT hr = S_FALSE;
    uint16_t      inWidth = 0;
    uint16_t      inHeight = 0;
    intptr_t      surfIndex = 0;
    ID3D11Texture2D* texture = nullptr;
    D3D11_TEXTURE2D_DESC texture_desc;
    RECT rect = { 0 };
    char szBuffer[128] = { 0 };
    VPE_FUNCTION functionParams;
    VPE_VERSION vpeVersion = {};
    VPE_MODE    vpeMode = {};
    SR_SCALING_MODE srScalingParams = {};
    VPE_SR_PARAMS srParams = {};

    const GUID* pExtensionGuid = nullptr;
    UINT        DataSize       = 0;
    void*       pData          = nullptr;
    D3D11_VIDEO_PROCESSOR_STREAM stream_;

    if (verbose == 1) {
        if (nullptr == pLogFile) {
            int err = fopen_s(&pLogFile, "C:\\temp\\Tencentlog2.txt", "w+t");
            if (err < 0) {
                //printf("%s:%d\r\n", __FUNCTION__, __LINE__);
                pLogFile = nullptr;
            }
            else{
                if (pLogFile != nullptr) {
                    sprintf_s(szBuffer, "SuperResolution function called \r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
           }
        }
#if 0
        if (nullptr == pOutputFile) {
            int err = fopen_s(&pOutputFile, "C:\\temp\\sr_dump2.nv12", "w+b");
            if (err < 0) {
                printf("%s:%d\r\n", __FUNCTION__, __LINE__);
            }
        }
#endif
    }
    if (nullptr == avframe || width == 0 || height == 0 ) {
        if (nullptr != pLogFile) {
            memset(szBuffer, 0, 128);
            sprintf_s(szBuffer, "Invalid parameter: width=%d, height=%d \r", width, height);
            fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
        }
        return -1;
    }

    hr = S_OK;
    texture = (ID3D11Texture2D*)avframe->data[0];
    surfIndex = (intptr_t)(avframe->data[1]);
  
    inWidth = avframe->width;// texture_desc.Width;
    inHeight = avframe->height;// texture_desc.Height;

    if (render_device) 
    {
        ID3D11Device* pTempDevice;
        texture->GetDevice(&pTempDevice);
        if (render_device != pTempDevice) {
            if (nullptr != pLogFile) {
                memset(szBuffer, 0, 128);
                sprintf_s(szBuffer, "Context Switch happen \r");
                fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
            }  
            resetContext();
       
        }
    }
  
    if (false == InitContext(texture, inWidth, inHeight, width, height)) {
        goto fail;
    }

    if (SUCCEEDED(hr)) {
        if (nullptr == srOutputTexture) {
            // Create output view and input view
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc;
            memset(&output_view_desc, 0, sizeof(output_view_desc));

            output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            output_view_desc.Texture2D.MipSlice = 0;

            D3D11_TEXTURE2D_DESC desc;
            memset(&desc, 0, sizeof(D3D11_TEXTURE2D_DESC));
            desc.Width  = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = 0;
            hr = render_device->CreateTexture2D(&desc, NULL, &srOutputTexture);
            if (FAILED(hr)) {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "failed to create SR D3D11 output texture\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
                goto fail;
            }
            else {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "create SR D3D11 output texture success\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
            }
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.BindFlags = 0;


            hr = render_device->CreateTexture2D(&desc, NULL, &srOutputStaging);
            if (FAILED(hr)) {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Failed to create SR D3D11 output staging texture \r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
                goto fail;
            }
            else {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "create SR D3D11 output staging texture success\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
            }

            hr = render_video_device->CreateVideoProcessorOutputView(
                srOutputTexture, video_processors_enum_, &output_view_desc,
                &output_view_);

            if (FAILED(hr)) {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Create Output  Texture view Failed\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
                goto fail;
            }
            else {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Create Output  Texture view success\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
            }
        }

        if (SUCCEEDED(hr)) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc;
            memset(&input_view_desc, 0, sizeof(input_view_desc));
            input_view_desc.FourCC = 0;
            input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            input_view_desc.Texture2D.MipSlice = 0;
            input_view_desc.Texture2D.ArraySlice = surfIndex;

            hr = render_video_device->CreateVideoProcessorInputView(
                texture, video_processors_enum_, &input_view_desc, &input_view_);

            if (SUCCEEDED(hr)) {
                // Blit NV12 surface to RGB back buffer here.
                memset(&stream_, 0, sizeof(stream_));
                stream_.Enable = true;
                stream_.OutputIndex = 0;
                stream_.InputFrameOrField = 0;
                stream_.PastFrames = 0;
                stream_.ppPastSurfaces = nullptr;
                stream_.ppFutureSurfaces = nullptr;
                stream_.pInputSurface = input_view_;
                stream_.ppPastSurfacesRight = nullptr;
                stream_.ppFutureSurfacesRight = nullptr;
                stream_.pInputSurfaceRight = nullptr;

                render_video_context->VideoProcessorSetStreamFrameFormat(
                    video_processor_, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            }
            else {
                if (nullptr != pLogFile) {
                    memset(szBuffer, 0, 128);
                    sprintf_s(szBuffer, "Create Input  Texture view failed\r");
                    fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
                }
                goto fail;
            }
        }
    }

    rect.left   = 0;
    rect.right  = inWidth;
    rect.top    = 0;
    rect.bottom = inHeight;
    render_video_context->VideoProcessorSetStreamSourceRect(video_processor_, 0, true, &rect);

    rect.left   = 0;
    rect.right  = width;
    rect.top    = 0;
    rect.bottom = height;
    render_video_context->VideoProcessorSetStreamDestRect(video_processor_, 0, true, &rect);

    // Set VPE Version
    memset((PVOID)&functionParams, 0, sizeof(functionParams));

    vpeVersion.Version = (UINT)VPE_VERSION_3_0;
    functionParams.Function = VPE_FN_SET_VERSION_PARAM;
    functionParams.pVPEVersion = &vpeVersion;

    pData = &functionParams;
    DataSize = sizeof(functionParams);
    pExtensionGuid = &GUID_VPE_INTERFACE;
    hr = render_video_context->VideoProcessorSetOutputExtension(video_processor_, pExtensionGuid, DataSize, pData);
    if (FAILED(hr)) {
        if (nullptr != pLogFile) {
            memset(szBuffer, 0, 128);
            sprintf_s(szBuffer, "failed to set version\r");
            fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
        }
        goto fail;
    }

    // Set SR Params
    memset((PVOID)&functionParams, 0, sizeof(functionParams));

    srParams.bEnable         = true;
    srParams.SRMode          = CAMERA_SCENARIO_MODE;
    functionParams.Function  = VPE_FN_SR_SET_PARAMS;
    functionParams.pSRParams = &srParams;

    pData = &functionParams;
    DataSize = sizeof(functionParams);
    pExtensionGuid = &GUID_VPE_INTERFACE;

    hr = render_video_context->VideoProcessorSetOutputExtension(video_processor_, pExtensionGuid, DataSize, pData);
    if (FAILED(hr)) {
        if (nullptr != pLogFile) {
            memset(szBuffer, 0, 128);
            sprintf_s(szBuffer, "failed to set output extension for SR mode\r");
            fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
        }
      return -1;
    }

    if (SUCCEEDED(hr)) {
        hr = render_video_context->VideoProcessorBlt(video_processor_, output_view_,
            0, 1, &stream_);
        if (FAILED(hr)) {
            if (nullptr != pLogFile) {
                memset(szBuffer, 0, 128);
                sprintf_s(szBuffer, "VideoProcessorBlt Failed hr=0x%x \r", hr);
                fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
            }
            return -1;
        }
        else
        {
            if (nullptr != pLogFile) {
                memset(szBuffer, 0, 128);
                sprintf_s(szBuffer, "VideoProcessorBlt success \r");
                fwrite(szBuffer, 1, strlen(szBuffer), pLogFile);
            }
        }
    }
    renderIndex++;
    render_device_ctx->CopyResource(srOutputStaging, srOutputTexture);
    D3D11_MAPPED_SUBRESOURCE subres;
    render_device_ctx->Map(srOutputStaging, 0, D3D11_MAP_READ, 0, &subres);
 
    for (int row = 0; row < height; row++) {
        memcpy(out[ncurrIndex] + width * row, (unsigned char*)((unsigned char*)(subres.pData) + subres.RowPitch * row), width);
    }

    for (int row = 0; row < height / 2; row++) {
        memcpy(out[ncurrIndex] + width * height + (width)*row, (unsigned char*)((unsigned char*)(subres.pData) + width * height + subres.RowPitch * row), width);
    }

    outavframe->data[0]     = out[ncurrIndex];
    outavframe->data[1]     = out[ncurrIndex] + width * height;
    outavframe->linesize[0] = width;
    outavframe->linesize[1] = width;
    outavframe->channels = 2;
    outavframe->format   = AV_PIX_FMT_NV12;
    outavframe->chroma_location = AVCHROMA_LOC_LEFT;
    outavframe->pkt_size = 1291555;
    outavframe->width    = width;
    outavframe->height   = height;

    outavframe->crop_top   = 0;
    outavframe->crop_bottom = 0;
    outavframe->crop_left = 0;
    outavframe->crop_right = 0;
    outavframe->sample_aspect_ratio.den = 1;
    outavframe->sample_aspect_ratio.num = 0;

    if (pOutputFile != nullptr) {
        fwrite(out[ncurrIndex], 1, height * width * 3 / 2, pOutputFile);
    }
    render_device_ctx->Unmap(srOutputStaging, 0);
    ncurrIndex++;
    if (ncurrIndex >= 4) {
        ncurrIndex = 0;
    }

    return 0;
fail:
    return -1;
}


