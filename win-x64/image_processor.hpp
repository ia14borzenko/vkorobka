#ifndef IMAGE_PROCESSOR_HPP
#define IMAGE_PROCESSOR_HPP

#include <vector>
#include <string>

#include <windows.h>
#include <gdiplus.h>
#include <iostream>
#include <algorithm>
#include "my_types.h"


// Функция для отзеркаливания JPG изображения по горизонтали
// Использует Windows GDI+ для декодирования/кодирования JPG
bool mirror_jpeg_horizontal(const std::vector<u8>& input_jpeg, std::vector<u8>& output_jpeg);

// Вспомогательная функция для получения CLSID кодека (объявление)
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);

#endif // IMAGE_PROCESSOR_HPP
