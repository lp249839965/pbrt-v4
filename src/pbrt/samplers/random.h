
/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_SAMPLERS_RANDOM_H
#define PBRT_SAMPLERS_RANDOM_H

// samplers/random.h*
#include <pbrt/util/rng.h>
#include <pbrt/core/sampler.h>

#include <memory>

namespace pbrt {

class RandomSampler : public Sampler {
  public:
    RandomSampler(int ns);
    void StartSequence(const Point2i &p, int sampleIndex);
    Float Get1D();
    Point2f Get2D();
    void Request1DArray(int n) final;
    void Request2DArray(int n) final;
    absl::Span<const Float> Get1DArray(int n) final;
    absl::Span<const Point2f> Get2DArray(int n) final;
    std::unique_ptr<Sampler> Clone();

  private:
    RNG rng;
    size_t array1DOffset, array2DOffset;
    std::vector<std::vector<Float>> sampleArray1D;
    std::vector<std::vector<Point2f>> sampleArray2D;
};

std::unique_ptr<RandomSampler> CreateRandomSampler(const ParamSet &params);

}  // namespace pbrt

#endif  // PBRT_SAMPLERS_RANDOM_H