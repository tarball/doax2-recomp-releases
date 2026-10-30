#include "util/image.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace util
{
    bool DecodeImage(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, bool premultiplied, std::vector<uint8_t>& bgra)
    {
        using Microsoft::WRL::ComPtr;
        if (!data || !size || size > 0x7FFFFFFF)
            return false;
        // COM on this thread for the call, unless it's set up already (in either model).
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> premultiply;
            UINT ownWidth = 0, ownHeight = 0;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size))) &&
                SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&ownWidth, &ownHeight)) && ownWidth && ownHeight &&
                ownWidth <= 16384 && ownHeight <= 16384 && SUCCEEDED(factory->CreateFormatConverter(&premultiply)) &&
                SUCCEEDED(premultiply->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                ComPtr<IWICBitmapSource> source = premultiply;
                if (!width || !height || width > 16384 || height > 16384)
                {
                    width = ownWidth;
                    height = ownHeight;
                }
                ComPtr<IWICBitmapScaler> scaler;
                bool scaled = true;
                if (width != ownWidth || height != ownHeight)
                {
                    // Fant (an area average) to shrink, cubic to enlarge.
                    const WICBitmapInterpolationMode mode = width < ownWidth && height < ownHeight ? WICBitmapInterpolationModeFant
                                                                                                    : WICBitmapInterpolationModeHighQualityCubic;
                    scaled = SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(source.Get(), width, height, mode));
                    if (scaled)
                        source = scaler;
                }
                ComPtr<IWICFormatConverter> straight;
                if (scaled && !premultiplied)
                {
                    scaled = SUCCEEDED(factory->CreateFormatConverter(&straight)) &&
                        SUCCEEDED(straight->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                            WICBitmapPaletteTypeCustom));
                    if (scaled)
                        source = straight;
                }
                if (scaled)
                {
                    bgra.resize(size_t(width) * height * 4);
                    ok = SUCCEEDED(source->CopyPixels(nullptr, width * 4, UINT(bgra.size()), bgra.data()));
                }
            }
        }
        if (SUCCEEDED(initialized))
            CoUninitialize();
        return ok;
    }
}
