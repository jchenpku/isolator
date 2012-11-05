
#include "linalg.hpp"
#include "config.h"

#include <math.h>

#define POLY0(x, c0) (c0)
#define POLY1(x, c0, c1) (x * POLY0(x, c1) + c0)
#define POLY2(x, c0, c1, c2) (x * POLY1(x, c1, c2) + c0)
#define POLY3(x, c0, c1, c2, c3) (x * POLY2(x, c1, c2, c3) + c0)
#define POLY4(x, c0, c1, c2, c3, c4) (x * POLY3(x, c1, c2, c3, c4) + c0)
#define POLY5(x, c0, c1, c2, c3, c4, c5) (x * POLY4(x, c1, c2, c3, c4, c5) + c0)

static float fastlog2(float x_)
{
    if (x_ <= 0.0f) return -INFINITY;

    union {
        float   f;
        int32_t i;
    } x, y, one;

    one.f = 1.0f;
    x.f = x_;

    float e = (float) ((x.i >> 23) - 127);

    y.i = (x.i & 0x007FFFFF) | one.i;
    float m = y.f;

    float p = POLY5(m, 3.1157899f, -3.3241990f, 2.5988452f, -1.2315303f,  3.1821337e-1f, -3.4436006e-2f);

    p *= m - 1.0;

    return p + e;
}



/* AVX versions */
#if defined(HAVE_AVX) && defined(HAVE_IMMINTRIN_H) && defined(__AVX__)

#include <immintrin.h>

typedef union {
    __m256i a;
    __m128i b[2];
} m256i_m128i_t;

typedef union {
    __m256 a;
    __m128 b[2];
} m256_m128_t;

/* Macros for evaluating ploynomials. */
#define AVX_POLY0(x, c0) _mm256_set1_ps(c0)
#define AVX_POLY1(x, c0, c1) \
    _mm256_add_ps(_mm256_mul_ps(AVX_POLY0(x, c1), x), _mm256_set1_ps(c0))
#define AVX_POLY2(x, c0, c1, c2) \
    _mm256_add_ps(_mm256_mul_ps(AVX_POLY1(x, c1, c2), x), _mm256_set1_ps(c0))
#define AVX_POLY3(x, c0, c1, c2, c3) \
    _mm256_add_ps(_mm256_mul_ps(AVX_POLY2(x, c1, c2, c3), x), _mm256_set1_ps(c0))
#define AVX_POLY4(x, c0, c1, c2, c3, c4) \
    _mm256_add_ps(_mm256_mul_ps(AVX_POLY3(x, c1, c2, c3, c4), x), _mm256_set1_ps(c0))
#define AVX_POLY5(x, c0, c1, c2, c3, c4, c5) \
    _mm256_add_ps(_mm256_mul_ps(AVX_POLY4(x, c1, c2, c3, c4, c5), x), _mm256_set1_ps(c0))


/* Comptue log2 over an avx single vector. */
static __m256 avx_log2(__m256 x)
{
    /* TODO: this can be written using avx2 instructions, without resorting to
     * SSE for integer operations, but I won't bother until I actually have an
     * cpu I can test that on.
     * */

    /* extract the exponent */
    const __m128 c127 = _mm_set1_epi32(127);
    m256i_m128i_t i;
    m256_m128_t e;
    i.a = _mm256_castps_si256(x);
    e.b[0] = _mm_sub_epi32(_mm_srli_epi32(i.b[0], 23), c127);
    e.b[1] = _mm_sub_epi32(_mm_srli_epi32(i.b[1], 23), c127);
    e.a = _mm256_cvtepi32_ps(e.a);

    /* extract the mantissa */
    m256_m128_t m;
    const __m256 c1 = _mm256_set1_ps(1.0f);
    const __m128i mant_mask = _mm_set1_epi32(0x007FFFFF);
    m.b[0] = _mm_castsi128_ps(_mm_and_si128(i.b[0], mant_mask));
    m.b[1] = _mm_castsi128_ps(_mm_and_si128(i.b[1], mant_mask));
    m.a = _mm256_or_ps(m.a, c1);

    /* polynomial approximation on the mantissa */
    __m256 p = AVX_POLY5(m.a, 3.1157899f,
                             -3.3241990f,
                              2.5988452f,
                             -1.2315303f,
                              3.1821337e-1f,
                             -3.4436006e-2f);

    p = _mm256_add_ps(_mm256_mul_ps(p, _mm256_sub_ps(m.a, c1)), e.a);

    /* Handle the log(x) = -INFINITY, for x <= 0 cases. */
    p = _mm256_blendv_ps(p, _mm256_set1_ps(-INFINITY),
                         _mm256_cmp_ps(x, _mm256_setzero_ps(), _CMP_LE_OQ));

    return p;
}


float dotlog(const float* xs, const float* ys, const size_t n)
{
    union {
        __m256 v;
        float f[8];
    } ans;
    ans.v = _mm256_setzero_ps();

    __m256 xv, yv;
    size_t i;
    for (i = 0; i < n / 8; ++i) {
        xv = _mm256_load_ps(xs + 8 * i);
        yv = _mm256_load_ps(ys + 8 * i);
        ans.v = _mm256_add_ps(ans.v, _mm256_mul_ps(xv, avx_log2(yv)));
    }

    float fans = ans.f[0] + ans.f[1] + ans.f[2] + ans.f[3] +
                 ans.f[4] + ans.f[5] + ans.f[6] + ans.f[7];

    /* handle any overhang */
    i *= 8;
    switch (n % 8) {
        case 7: fans += xs[i] * fastlog2(ys[i]); ++i;
        case 6: fans += xs[i] * fastlog2(ys[i]); ++i;
        case 5: fans += xs[i] * fastlog2(ys[i]); ++i;
        case 4: fans += xs[i] * fastlog2(ys[i]); ++i;
        case 3: fans += xs[i] * fastlog2(ys[i]); ++i;
        case 2: fans += xs[i] * fastlog2(ys[i]); ++i;
        case 1: fans += xs[i] * fastlog2(ys[i]); ++i;
    }

    return fans;
}


//float asxpy(float* xs, const float* ys, const float c,
            //const unsigned int* idx, const size_t n)
//{

//}


/* Vanilla versions */
#else

float dotlog(const float* xs, const float* ys, const size_t n)
{
    float ans = 0.0;
    size_t i;
    for (i = 0; i < n; ++i) {
        ans += xs[i] * fastlog2(ys[i]);
    }
    return ans;
}


void asxpy(float* xs, const float* ys, const float c,
            const unsigned int* idx, const size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        xs[idx[i]] += c * ys[i];
    }
}



#endif