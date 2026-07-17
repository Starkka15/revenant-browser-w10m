/*
 * FFTFrame implementation for the Revenant WinCairo/UWP port.
 *
 * Self-contained iterative radix-2 Cooley-Tukey FFT. No platform DSP library
 * (no Accelerate/vDSP, no GStreamer gst_fft) is available on ARM32 UWP, so this
 * provides the real transform used by AnalyserNode, PeriodicWave/Oscillator,
 * the Convolver, and all other Web Audio consumers.
 *
 * Layout matches the "packed" convention that the cross-platform code in
 * FFTFrame.cpp (multiply(), scaleFFT(), interpolateFrequencyComponents()) is
 * written against — the same one FFTFrameMac uses:
 *
 *   realData[0]      = DC term       (X[0].re, imaginary part is 0 for real input)
 *   imagData[0]      = Nyquist term  (X[N/2].re, imaginary part is 0 for real input)
 *   realData[k]      = X[k].re   for k = 1 .. N/2-1
 *   imagData[k]      = X[k].im   for k = 1 .. N/2-1
 *
 * doFFT() produces the textbook DFT value X[k] directly (no extra scale), which
 * equals the value FFTFrameMac lands on after its /2 vDSP correction, so all
 * magnitude-sensitive consumers see identical numbers to the reference backend.
 * doInverseFFT() performs the true inverse (1/N) so IFFT(FFT(x)) == x.
 */

#include "config.h"

#if ENABLE(WEB_AUDIO) && !USE(ACCELERATE) && !USE(GSTREAMER)

#include "FFTFrame.h"

#include "VectorMath.h"
#include <cmath>
#include <utility>
#include <vector>
#include <wtf/MathExtras.h>

namespace WebCore {

namespace {

// In-place iterative radix-2 FFT over a full complex array of size n (a power of two).
// direction = -1 for forward (e^{-i...}), +1 for inverse (e^{+i...}). No normalization here.
static void complexFFT(float* re, float* im, unsigned n, int direction)
{
    if (n < 2)
        return;

    // Bit-reversal permutation.
    for (unsigned i = 1, j = 0; i < n; ++i) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    // Danielson-Lanczos butterflies.
    for (unsigned len = 2; len <= n; len <<= 1) {
        double ang = direction * 2.0 * piDouble / static_cast<double>(len);
        double wReal = std::cos(ang);
        double wImag = std::sin(ang);
        unsigned half = len >> 1;
        for (unsigned i = 0; i < n; i += len) {
            double curReal = 1.0;
            double curImag = 0.0;
            for (unsigned k = 0; k < half; ++k) {
                unsigned a = i + k;
                unsigned b = i + k + half;
                double bRe = re[b] * curReal - im[b] * curImag;
                double bIm = re[b] * curImag + im[b] * curReal;
                double aRe = re[a];
                double aIm = im[a];
                re[a] = static_cast<float>(aRe + bRe);
                im[a] = static_cast<float>(aIm + bIm);
                re[b] = static_cast<float>(aRe - bRe);
                im[b] = static_cast<float>(aIm - bIm);
                double nextReal = curReal * wReal - curImag * wImag;
                curImag = curReal * wImag + curImag * wReal;
                curReal = nextReal;
            }
        }
    }
}

} // anonymous namespace

// Normal constructor: allocates for a given fftSize.
FFTFrame::FFTFrame(unsigned fftSize)
    : m_FFTSize(fftSize)
    , m_log2FFTSize(static_cast<unsigned>(log2(fftSize)))
    , m_realData(fftSize)
    , m_imagData(fftSize)
{
    // Only powers of two are supported.
    ASSERT(1UL << m_log2FFTSize == m_FFTSize);
    m_realData.zero();
    m_imagData.zero();
}

// Creates a blank/empty frame (interpolate() must later be called).
FFTFrame::FFTFrame()
    : m_FFTSize(0)
    , m_log2FFTSize(0)
{
}

// Copy constructor.
FFTFrame::FFTFrame(const FFTFrame& frame)
    : m_FFTSize(frame.m_FFTSize)
    , m_log2FFTSize(frame.m_log2FFTSize)
    , m_realData(frame.m_FFTSize)
    , m_imagData(frame.m_FFTSize)
{
    memcpy(realData().data(), frame.realData().data(), sizeof(float) * m_FFTSize);
    memcpy(imagData().data(), frame.imagData().data(), sizeof(float) * m_FFTSize);
}

void FFTFrame::initialize()
{
}

FFTFrame::~FFTFrame()
{
}

void FFTFrame::doFFT(const float* data)
{
    unsigned n = m_FFTSize;
    unsigned halfSize = n / 2;

    RELEASE_ASSERT(realData().size() >= halfSize);
    RELEASE_ASSERT(imagData().size() >= halfSize);

    // Load the real signal into a full complex scratch buffer (imag = 0) and transform.
    std::vector<float> re(n), im(n, 0.0f);
    memcpy(re.data(), data, sizeof(float) * n);

    complexFFT(re.data(), im.data(), n, -1);

    // Pack into the halfSize real/imag arrays (DC in real[0], Nyquist in imag[0]).
    float* realP = realData().data();
    float* imagP = imagData().data();

    realP[0] = re[0];          // X[0] is real for real input.
    imagP[0] = re[halfSize];   // X[N/2] (Nyquist) is real for real input.
    for (unsigned k = 1; k < halfSize; ++k) {
        realP[k] = re[k];
        imagP[k] = im[k];
    }
}

void FFTFrame::doInverseFFT(float* data)
{
    unsigned n = m_FFTSize;
    unsigned halfSize = n / 2;

    const float* realP = realData().data();
    const float* imagP = imagData().data();

    // Rebuild the full Hermitian-symmetric complex spectrum from the packed halves.
    std::vector<float> re(n), im(n);

    re[0] = realP[0];          // DC
    im[0] = 0.0f;
    re[halfSize] = imagP[0];   // Nyquist (stored packed in imag[0])
    im[halfSize] = 0.0f;
    for (unsigned k = 1; k < halfSize; ++k) {
        re[k] = realP[k];
        im[k] = imagP[k];
        re[n - k] = realP[k];   // conjugate symmetry
        im[n - k] = -imagP[k];
    }

    complexFFT(re.data(), im.data(), n, +1);

    // Take the real part and scale by 1/N so that x == IFFT(FFT(x)).
    float scale = 1.0f / static_cast<float>(n);
    for (unsigned i = 0; i < n; ++i)
        data[i] = re[i] * scale;
}

int FFTFrame::minFFTSize()
{
    return 1 << 2;
}

int FFTFrame::maxFFTSize()
{
    return 1 << 24;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && !USE(ACCELERATE) && !USE(GSTREAMER)
