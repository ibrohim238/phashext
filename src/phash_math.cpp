/*
 * Математика pHash над изображением, наружу отдаётся расширением phashext (см. ext.cpp).
 * Каждая функция повторяет арифметику эталона на Python бит в бит:
 *
 *   phash_luma_from_gd_truecolor — RGB → L по ITU-R 601-2, как PIL.Image.convert('L') (Convert.c, L24);
 *   phash_png_unfilter           — снятие построчных фильтров PNG (RFC 2083, 6);
 *   phash_png_luma8              — яркость строк 8-битного PNG;
 *   pillow_resample_lanczos      — фрагменты Pillow 9.4.0 src/libImaging/Resample.c для 8-битного
 *                                  режима L: lanczos_filter, precompute_coeffs, normalize_coeffs_8bpc,
 *                                  ImagingResampleHorizontal/Vertical_8bpc, ImagingResampleInner;
 *   phash_dct_low_frequencies    — DCT-II через scipy/pocketfft_hdronly.h, дословную копию
 *                                  scipy/fft/_pocketfft/pocketfft_hdronly.h из scipy v1.10.0.
 *
 * Сборка: см. README (-ffp-contract=off запрещает FMA, чтобы порядок округлений не отличался от эталона).
 */
#include "scipy/pocketfft_hdronly.h"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

extern "C" {

/* ────────────────────────────── яркость ────────────────────────────── */

/* argb — пиксели формата .gd (imagegd): 4 байта big-endian, A R G B */
void
phash_luma_from_gd_truecolor(const unsigned char *argb, int count, unsigned char *out) {
    int i;
    for (i = 0; i < count; i++, argb += 4) {
        out[i] = (unsigned char)((argb[1] * 19595 + argb[2] * 38470 + argb[3] * 7471 + 0x8000) >> 16);
    }
}

/* ────────────────────────────── построчные фильтры PNG ────────────────────────────── */

static inline int
paeth_predictor(int a, int b, int c) {
    int p  = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);

    if (pa <= pb && pa <= pc) {
        return a;
    }

    return pb <= pc ? b : c;
}

/*
 * data — распакованные строки подряд, по (1 + rowBytes) байт: первый байт строки задаёт тип фильтра,
 * остальные фильтруются на месте сверху вниз. Предыдущей строкой для первой считается нулевая.
 * Возвращает -1, если тип фильтра неизвестен.
 */
int
phash_png_unfilter(unsigned char *data, int rowBytes, int rows, int pixelBytes) {
    const unsigned char *previous = NULL;
    int                  y, i;

    for (y = 0; y < rows; y++) {
        unsigned char *line   = data + (size_t)y * (rowBytes + 1);
        int            filter = line[0];

        line++;

        for (i = 0; i < rowBytes; i++) {
            int a = i >= pixelBytes ? line[i - pixelBytes] : 0;
            int b = previous != NULL ? previous[i] : 0;
            int c = (previous != NULL && i >= pixelBytes) ? previous[i - pixelBytes] : 0;
            int predictor;

            switch (filter) {
                case 0:  predictor = 0; break;
                case 1:  predictor = a; break;
                case 2:  predictor = b; break;
                case 3:  predictor = (a + b) >> 1; break;
                case 4:  predictor = paeth_predictor(a, b, c); break;
                default: return -1;
            }

            line[i] = (unsigned char)((line[i] + predictor) & 0xFF);
        }

        previous = line;
    }

    return 0;
}

/* Байт на пиксель 8-битного PNG по типу цвета, -1 — тип не поддерживается */
int
phash_png_bytes_per_pixel(int color) {
    switch (color) {
        case 0:  return 1;
        case 2:  return 3;
        case 3:  return 1;
        case 4:  return 2;
        case 6:  return 4;
        default: return -1;
    }
}

/*
 * Яркость строк 8-битного PNG как у Pillow: серый и серый с альфой берут первый байт пикселя,
 * палитровые — яркость индекса из paletteLuma, RGB и RGBA — ITU-R 601-2 по трём первым каналам.
 * data — строки после phash_png_unfilter, out — width * rows байт яркости. -1 при неизвестном типе цвета.
 */
int
phash_png_luma8(
    const unsigned char *data, int rowBytes, int rows, int width, int color,
    const unsigned char *paletteLuma, unsigned char *out
) {
    int step = phash_png_bytes_per_pixel(color);
    int y, x;

    if (step < 0) {
        return -1;
    }

    for (y = 0; y < rows; y++) {
        const unsigned char *line = data + (size_t)y * (rowBytes + 1) + 1;
        unsigned char       *row  = out + (size_t)y * width;

        for (x = 0; x < width; x++) {
            const unsigned char *pixel = line + (size_t)x * step;

            if (color == 2 || color == 6) {
                row[x] = (unsigned char)((pixel[0] * 19595 + pixel[1] * 38470 + pixel[2] * 7471 + 0x8000) >> 16);
            } else if (color == 3) {
                row[x] = paletteLuma[pixel[0]];
            } else {
                row[x] = pixel[0];
            }
        }
    }

    return 0;
}

/* ────────────────────────────── Lanczos-ресайз (Pillow) ────────────────────────────── */

#define LANCZOS_SUPPORT 3.0

/* 8 bits for result. Filter can have negative areas.
   In one cases the sum of the coefficients will be negative,
   in the other it will be more than 1.0. That is why we need
   two extra bits for overflow and int type. */
#define PRECISION_BITS (32 - 8 - 2)

static inline double
sinc_filter(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    x = x * M_PI;
    return sin(x) / x;
}

static inline double
lanczos_filter(double x) {
    /* truncated sinc */
    if (-3.0 <= x && x < 3.0) {
        return sinc_filter(x) * sinc_filter(x / 3);
    }
    return 0.0;
}

/* Pillow берёт clip8_lookups[in >> PRECISION_BITS]: 0 для отрицательных, 255 для > 255 */
static inline unsigned char
clip8(int in) {
    int value = in >> PRECISION_BITS;
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (unsigned char)value;
}

static int
precompute_coeffs(int inSize, float in0, float in1, int outSize, int **boundsp, double **kkp) {
    double support, scale, filterscale;
    double center, ww, ss;
    int xx, x, ksize, xmin, xmax;
    int *bounds;
    double *kk, *k;

    /* пустой размер: дальше на него делится масштаб */
    if (inSize <= 0 || outSize <= 0) {
        return 0;
    }

    /* prepare for horizontal stretch */
    filterscale = scale = (double)(in1 - in0) / outSize;
    if (filterscale < 1.0) {
        filterscale = 1.0;
    }

    /* determine support size (length of resampling filter) */
    support = LANCZOS_SUPPORT * filterscale;

    /* maximum number of coeffs */
    ksize = (int)ceil(support) * 2 + 1;

    // check for overflow
    if (outSize > INT_MAX / (ksize * (int)sizeof(double))) {
        return 0;
    }

    /* coefficient buffer */
    kk = (double *)malloc(outSize * ksize * sizeof(double));
    if (!kk) {
        return 0;
    }

    bounds = (int *)malloc(outSize * 2 * sizeof(int));
    if (!bounds) {
        free(kk);
        return 0;
    }

    for (xx = 0; xx < outSize; xx++) {
        center = in0 + (xx + 0.5) * scale;
        ww = 0.0;
        ss = 1.0 / filterscale;
        // Round the value
        xmin = (int)(center - support + 0.5);
        if (xmin < 0) {
            xmin = 0;
        }
        // Round the value
        xmax = (int)(center + support + 0.5);
        if (xmax > inSize) {
            xmax = inSize;
        }
        xmax -= xmin;
        k = &kk[xx * ksize];
        for (x = 0; x < xmax; x++) {
            double w = lanczos_filter((x + xmin - center + 0.5) * ss);
            k[x] = w;
            ww += w;
        }
        for (x = 0; x < xmax; x++) {
            if (ww != 0.0) {
                k[x] /= ww;
            }
        }
        // Remaining values should stay empty if they are used despite of xmax.
        for (; x < ksize; x++) {
            k[x] = 0;
        }
        bounds[xx * 2 + 0] = xmin;
        bounds[xx * 2 + 1] = xmax;
    }
    *boundsp = bounds;
    *kkp = kk;
    return ksize;
}

static void
normalize_coeffs_8bpc(int outSize, int ksize, double *prekk) {
    int x;
    int *kk;

    // use the same buffer for normalized coefficients
    kk = (int *)prekk;

    for (x = 0; x < outSize * ksize; x++) {
        if (prekk[x] < 0) {
            kk[x] = (int)(-0.5 + prekk[x] * (1 << PRECISION_BITS));
        } else {
            kk[x] = (int)(0.5 + prekk[x] * (1 << PRECISION_BITS));
        }
    }
}

/* imOut: outWidth × outHeight; imIn: строки шириной inWidth, начиная со строки offset */
static void
resample_horizontal_8bpc(
    unsigned char *imOut, int outWidth, int outHeight,
    const unsigned char *imIn, int inWidth, int offset,
    int ksize, int *bounds, double *prekk) {
    int ss0;
    int xx, yy, x, xmin, xmax;
    int *k, *kk;

    // use the same buffer for normalized coefficients
    kk = (int *)prekk;
    normalize_coeffs_8bpc(outWidth, ksize, prekk);

    for (yy = 0; yy < outHeight; yy++) {
        for (xx = 0; xx < outWidth; xx++) {
            xmin = bounds[xx * 2 + 0];
            xmax = bounds[xx * 2 + 1];
            k = &kk[xx * ksize];
            ss0 = 1 << (PRECISION_BITS - 1);
            for (x = 0; x < xmax; x++) {
                ss0 += ((unsigned char)imIn[(yy + offset) * inWidth + x + xmin]) * k[x];
            }
            imOut[yy * outWidth + xx] = clip8(ss0);
        }
    }
}

/* imOut: width × outHeight; imIn: строки той же ширины width */
static void
resample_vertical_8bpc(
    unsigned char *imOut, int width, int outHeight,
    const unsigned char *imIn,
    int ksize, int *bounds, double *prekk) {
    int ss0;
    int xx, yy, y, ymin, ymax;
    int *k, *kk;

    // use the same buffer for normalized coefficients
    kk = (int *)prekk;
    normalize_coeffs_8bpc(outHeight, ksize, prekk);

    for (yy = 0; yy < outHeight; yy++) {
        k = &kk[yy * ksize];
        ymin = bounds[yy * 2 + 0];
        ymax = bounds[yy * 2 + 1];
        for (xx = 0; xx < width; xx++) {
            ss0 = 1 << (PRECISION_BITS - 1);
            for (y = 0; y < ymax; y++) {
                ss0 += ((unsigned char)imIn[(y + ymin) * width + xx]) * k[y];
            }
            imOut[yy * width + xx] = clip8(ss0);
        }
    }
}

/* ImagingResampleInner для одного 8-битного канала и box = (0, 0, inWidth, inHeight). 0 — успех, -1 — пустой размер или нет памяти */
int
pillow_resample_lanczos(
    const unsigned char *in, int inWidth, int inHeight,
    unsigned char *out, int outWidth, int outHeight) {
    unsigned char *imTemp = NULL;
    const unsigned char *imIn = in;
    int i, need_horizontal, need_vertical;
    int ybox_first, ybox_last;
    int ksize_horiz, ksize_vert;
    int *bounds_horiz, *bounds_vert;
    double *kk_horiz, *kk_vert;

    need_horizontal = outWidth != inWidth;
    need_vertical = outHeight != inHeight;

    ksize_horiz = precompute_coeffs(inWidth, 0, (float)inWidth, outWidth, &bounds_horiz, &kk_horiz);
    if (!ksize_horiz) {
        return -1;
    }

    ksize_vert = precompute_coeffs(inHeight, 0, (float)inHeight, outHeight, &bounds_vert, &kk_vert);
    if (!ksize_vert) {
        free(bounds_horiz);
        free(kk_horiz);
        return -1;
    }

    // First used row in the source image
    ybox_first = bounds_vert[0];
    // Last used row in the source image
    ybox_last = bounds_vert[outHeight * 2 - 2] + bounds_vert[outHeight * 2 - 1];

    /* two-pass resize, horizontal pass */
    if (need_horizontal) {
        // Shift bounds for vertical pass
        for (i = 0; i < outHeight; i++) {
            bounds_vert[i * 2] -= ybox_first;
        }

        imTemp = (unsigned char *)malloc((size_t)outWidth * (ybox_last - ybox_first));
        if (imTemp) {
            resample_horizontal_8bpc(
                imTemp, outWidth, ybox_last - ybox_first,
                in, inWidth, ybox_first,
                ksize_horiz, bounds_horiz, kk_horiz);
        }
        free(bounds_horiz);
        free(kk_horiz);
        if (!imTemp) {
            free(bounds_vert);
            free(kk_vert);
            return -1;
        }
        imIn = imTemp;
    } else {
        free(bounds_horiz);
        free(kk_horiz);
    }

    /* vertical pass */
    if (need_vertical) {
        /* imIn can be the original image or horizontally resampled one */
        resample_vertical_8bpc(out, outWidth, outHeight, imIn, ksize_vert, bounds_vert, kk_vert);
    } else {
        /* только горизонтальный проход либо копирование без ресайза */
        memcpy(out, imIn, (size_t)outWidth * outHeight);
    }

    free(imTemp);
    free(bounds_vert);
    free(kk_vert);

    return 0;
}

/* Повторяет scipy.fftpack.dct(scipy.fftpack.dct(pixels, axis=0), axis=1) */
int
phash_dct_low_frequencies(const unsigned char *pixels, int size, int hashSize, double *out) {
    if (size <= 0 || hashSize <= 0 || hashSize > size) {
        return -1;
    }

    const size_t n = static_cast<size_t>(size);

    try {
        const size_t rows = static_cast<size_t>(hashSize);

        std::unique_ptr<double[]> input(new double[n * n]);
        std::unique_ptr<double[]> byColumns(new double[n * n]);
        std::unique_ptr<double[]> byRows(new double[rows * n]);
        for (size_t i = 0; i < n * n; ++i) {
            input[i] = static_cast<double>(pixels[i]);
        }

        const pocketfft::stride_t stride{
            static_cast<ptrdiff_t>(n * sizeof(double)),
            static_cast<ptrdiff_t>(sizeof(double)),
        };

        pocketfft::dct(pocketfft::shape_t{n, n}, stride, stride, pocketfft::shape_t{0}, 2,
                       input.get(), byColumns.get(), 1.0, false);

        /* по строкам преобразования независимы, поэтому считаем только те hashSize, что попадут в хеш */
        pocketfft::dct(pocketfft::shape_t{rows, n}, stride, stride, pocketfft::shape_t{1}, 2,
                       byColumns.get(), byRows.get(), 1.0, false);

        for (size_t k = 0; k < rows; ++k) {
            std::memcpy(&out[k * rows], &byRows[k * n], rows * sizeof(double));
        }
    } catch (...) {
        return -1;
    }

    return 0;
}
}
