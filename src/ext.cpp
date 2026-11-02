/*
 * Расширение PHP поверх математики из phash_math.cpp: те же функции, вызываемые из PHP напрямую.
 * Потребитель — пакет ibrohim238/phash, без расширения он не работает. Сборка: make, подробности в README.
 */
#include <phpcpp.h>

#include <string>
#include <vector>

extern "C" {
void phash_luma_from_gd_truecolor(const unsigned char *argb, int count, unsigned char *out);
int phash_png_unfilter(unsigned char *data, int rowBytes, int rows, int pixelBytes);
int phash_png_bytes_per_pixel(int color);
int phash_png_luma8(const unsigned char *data, int rowBytes, int rows, int width, int color,
                    const unsigned char *paletteLuma, unsigned char *out);
int pillow_resample_lanczos(const unsigned char *in, int inWidth, int inHeight,
                            unsigned char *out, int outWidth, int outHeight);
int phash_dct_low_frequencies(const unsigned char *pixels, int size, int hashSize, double *out);
}

/*
 * Функции регистрируются в PHP глобально, поэтому размеры из параметров сверяются с длиной переданной
 * строки: C-код пишет и читает по ним без проверок, и несогласованный вызов ушёл бы за границу буфера.
 */
static void requireInRange(bool condition, const char *function)
{
    if (!condition) {
        throw Php::Exception(std::string(function) + ": размеры не согласованы с данными");
    }
}

/* Байт в строках PNG: rows строк по (1 + rowBytes), первый байт строки — тип фильтра */
static size_t pngRowsSize(int rowBytes, int rows)
{
    return static_cast<size_t>(rows) * (static_cast<size_t>(rowBytes) + 1);
}

static Php::Value extLuma(Php::Parameters &params)
{
    const std::string argb = params[0];
    const int count = static_cast<int>(argb.size() / 4);

    std::string out(static_cast<size_t>(count), '\0');
    phash_luma_from_gd_truecolor(reinterpret_cast<const unsigned char *>(argb.data()), count,
                                 reinterpret_cast<unsigned char *>(&out[0]));

    return out;
}

static Php::Value extPngUnfilter(Php::Parameters &params)
{
    std::string data = params[0];
    const int rowBytes = params[1];
    const int rows = params[2];
    const int pixelBytes = params[3];

    requireInRange(rowBytes >= 0 && rows >= 0 && pixelBytes > 0 && pngRowsSize(rowBytes, rows) <= data.size(),
                   "phashext_png_unfilter");

    if (phash_png_unfilter(reinterpret_cast<unsigned char *>(&data[0]), rowBytes, rows, pixelBytes) != 0) {
        return nullptr;
    }

    return data;
}

static Php::Value extPngLuma(Php::Parameters &params)
{
    const std::string data = params[0];
    const int rowBytes = params[1];
    const int rows = params[2];
    const int width = params[3];
    const int color = params[4];
    std::string paletteLuma = params[5];
    paletteLuma.resize(256, '\0');

    const int pixelBytes = phash_png_bytes_per_pixel(color);
    if (pixelBytes < 0) {
        throw Php::Exception("phash_png_luma8: неизвестный тип цвета PNG");
    }

    requireInRange(rowBytes >= 0 && rows >= 0 && width >= 0
                       && static_cast<size_t>(width) * pixelBytes <= static_cast<size_t>(rowBytes)
                       && pngRowsSize(rowBytes, rows) <= data.size(),
                   "phashext_png_luma");

    std::string out(static_cast<size_t>(width) * rows, '\0');
    phash_png_luma8(reinterpret_cast<const unsigned char *>(data.data()), rowBytes, rows, width, color,
                    reinterpret_cast<const unsigned char *>(paletteLuma.data()),
                    reinterpret_cast<unsigned char *>(&out[0]));

    return out;
}

static Php::Value extResize(Php::Parameters &params)
{
    const std::string in = params[0];
    const int inWidth = params[1];
    const int inHeight = params[2];
    const int outWidth = params[3];
    const int outHeight = params[4];

    requireInRange(inWidth > 0 && inHeight > 0 && outWidth > 0 && outHeight > 0
                       && static_cast<size_t>(inWidth) * inHeight <= in.size(),
                   "phashext_resize");

    std::string out(static_cast<size_t>(outWidth) * outHeight, '\0');
    if (pillow_resample_lanczos(reinterpret_cast<const unsigned char *>(in.data()), inWidth, inHeight,
                                reinterpret_cast<unsigned char *>(&out[0]), outWidth, outHeight) != 0) {
        throw Php::Exception("pillow_resample_lanczos: не удалось выделить память");
    }

    return out;
}

static Php::Value extDct(Php::Parameters &params)
{
    const std::string pixels = params[0];
    const int size = params[1];
    const int hashSize = params[2];

    requireInRange(size > 0 && hashSize > 0 && hashSize <= size
                       && static_cast<size_t>(size) * size <= pixels.size(),
                   "phashext_dct");

    std::vector<double> raw(static_cast<size_t>(hashSize) * hashSize);
    if (phash_dct_low_frequencies(reinterpret_cast<const unsigned char *>(pixels.data()), size, hashSize,
                                  raw.data()) != 0) {
        throw Php::Exception("phash_dct_low_frequencies: не удалось посчитать DCT");
    }

    Php::Value result;
    for (int k = 0; k < hashSize; ++k) {
        Php::Value row;
        for (int j = 0; j < hashSize; ++j) {
            row[j] = raw[static_cast<size_t>(k) * hashSize + j];
        }
        result[k] = row;
    }

    return result;
}

extern "C" {
PHPCPP_EXPORT void *get_module()
{
    static Php::Extension extension("phashext", "0.1.0");

    extension.add<extLuma>("phashext_luma", {Php::ByVal("argb", Php::Type::String)});
    extension.add<extPngUnfilter>("phashext_png_unfilter", {
        Php::ByVal("data", Php::Type::String),
        Php::ByVal("rowBytes", Php::Type::Numeric),
        Php::ByVal("rows", Php::Type::Numeric),
        Php::ByVal("pixelBytes", Php::Type::Numeric),
    });
    extension.add<extPngLuma>("phashext_png_luma", {
        Php::ByVal("data", Php::Type::String),
        Php::ByVal("rowBytes", Php::Type::Numeric),
        Php::ByVal("rows", Php::Type::Numeric),
        Php::ByVal("width", Php::Type::Numeric),
        Php::ByVal("color", Php::Type::Numeric),
        Php::ByVal("paletteLuma", Php::Type::String),
    });
    extension.add<extResize>("phashext_resize", {
        Php::ByVal("in", Php::Type::String),
        Php::ByVal("inWidth", Php::Type::Numeric),
        Php::ByVal("inHeight", Php::Type::Numeric),
        Php::ByVal("outWidth", Php::Type::Numeric),
        Php::ByVal("outHeight", Php::Type::Numeric),
    });
    extension.add<extDct>("phashext_dct", {
        Php::ByVal("pixels", Php::Type::String),
        Php::ByVal("size", Php::Type::Numeric),
        Php::ByVal("hashSize", Php::Type::Numeric),
    });

    return extension;
}
}
