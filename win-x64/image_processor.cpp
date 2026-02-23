#include "image_processor.hpp"
#include <windows.h>
#include <gdiplus.h>
#include <iostream>
#include <algorithm>
#include "my_types.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// Инициализация GDI+
static ULONG_PTR gdiplusToken = 0;
static bool gdiplusInitialized = false;

static void init_gdiplus()
{
    if (!gdiplusInitialized)
    {
        GdiplusStartupInput gdiplusStartupInput;
        GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
        gdiplusInitialized = true;
    }
}

static void shutdown_gdiplus()
{
    if (gdiplusInitialized)
    {
        GdiplusShutdown(gdiplusToken);
        gdiplusInitialized = false;
    }
}

bool mirror_jpeg_horizontal(const std::vector<u8>& input_jpeg, std::vector<u8>& output_jpeg)
{
    init_gdiplus();
    
    output_jpeg.clear();
    
    // Создаем поток из памяти
    IStream* stream = nullptr;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, input_jpeg.size());
    if (!hMem)
    {
        std::cerr << "[image] Failed to allocate memory for stream" << std::endl;
        return false;
    }
    
    void* pMem = GlobalLock(hMem);
    memcpy(pMem, input_jpeg.data(), input_jpeg.size());
    GlobalUnlock(hMem);
    
    if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK)
    {
        GlobalFree(hMem);
        std::cerr << "[image] Failed to create stream" << std::endl;
        return false;
    }
    
    // Загружаем изображение из потока
    Bitmap* bitmap = Bitmap::FromStream(stream);
    if (!bitmap || bitmap->GetLastStatus() != Ok)
    {
        stream->Release();
        std::cerr << "[image] Failed to load image from stream" << std::endl;
        return false;
    }
    
    // Получаем размеры
    UINT width = bitmap->GetWidth();
    UINT height = bitmap->GetHeight();
    
    // Создаем новое изображение для отзеркаливания
    Bitmap* mirrored = new Bitmap(width, height, PixelFormat24bppRGB);
    if (!mirrored || mirrored->GetLastStatus() != Ok)
    {
        delete bitmap;
        stream->Release();
        std::cerr << "[image] Failed to create mirrored bitmap" << std::endl;
        return false;
    }
    
    Graphics graphics(mirrored);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    
    // Применяем отзеркаливание через матрицу преобразования
    Matrix matrix(-1.0f, 0.0f, 0.0f, 1.0f, (REAL)width, 0.0f);
    graphics.SetTransform(&matrix);
    graphics.DrawImage(bitmap, 0, 0, width, height);
    
    // Сохраняем в JPG
    IStream* outputStream = nullptr;
    HGLOBAL hOutputMem = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!hOutputMem)
    {
        delete mirrored;
        delete bitmap;
        stream->Release();
        std::cerr << "[image] Failed to allocate memory for output stream" << std::endl;
        return false;
    }
    
    if (CreateStreamOnHGlobal(hOutputMem, FALSE, &outputStream) != S_OK)
    {
        GlobalFree(hOutputMem);
        delete mirrored;
        delete bitmap;
        stream->Release();
        std::cerr << "[image] Failed to create output stream" << std::endl;
        return false;
    }
    
    // Кодируем в JPG
    CLSID jpegClsid;
    GetEncoderClsid(L"image/jpeg", &jpegClsid);
    
    EncoderParameters encoderParams;
    encoderParams.Count = 1;
    encoderParams.Parameter[0].Guid = EncoderQuality;
    encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
    encoderParams.Parameter[0].NumberOfValues = 1;
    ULONG quality = 95;
    encoderParams.Parameter[0].Value = &quality;
    
    Status status = mirrored->Save(outputStream, &jpegClsid, &encoderParams);
    
    if (status == Ok)
    {
        // Читаем данные из потока
        STATSTG stat;
        if (outputStream->Stat(&stat, STATFLAG_NONAME) == S_OK)
        {
            LARGE_INTEGER li = {0};
            outputStream->Seek(li, STREAM_SEEK_SET, NULL);
            
            ULARGE_INTEGER uli = {0};
            uli.QuadPart = stat.cbSize.QuadPart;
            
            output_jpeg.resize(static_cast<size_t>(uli.QuadPart));
            ULONG bytesRead = 0;
            outputStream->Read(output_jpeg.data(), static_cast<ULONG>(output_jpeg.size()), &bytesRead);
            output_jpeg.resize(bytesRead);
        }
    }
    
    outputStream->Release();
    delete mirrored;
    delete bitmap;
    stream->Release();
    
    if (status != Ok)
    {
        std::cerr << "[image] Failed to save mirrored image" << std::endl;
        return false;
    }
    
    std::cout << "[image] Image mirrored successfully: " << input_jpeg.size() 
              << " bytes -> " << output_jpeg.size() << " bytes" << std::endl;
    
    return true;
}


// Вспомогательная функция для получения CLSID кодека
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0;
    UINT size = 0;
    
    ImageCodecInfo* pImageCodecInfo = NULL;
    
    GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;
    
    pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL)
        return -1;
    
    GetImageEncoders(num, size, pImageCodecInfo);
    
    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
        {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    
    free(pImageCodecInfo);
    return -1;
}
