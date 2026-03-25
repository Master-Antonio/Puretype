#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace puretype
{
    struct EdgeAdaptiveParams
    {
        float verticalGradBias;
        float verticalRiskGain;
        float edgeRiskVerticalW;
        float edgeRiskSpreadW;
        float edgeRiskThinLow;
        float chromaEdgeAtten;
        float chromaPenaltyBase;
        float chromaPenaltyEdge;
        float chromaPenaltyContrast;
    };

    struct ConstrainedChromaFastPath
    {
        bool enabled = true;
        float maxEdgeRisk = 0.08f;
        float maxChannelSpread = 0.06f;
        float maxLumaDelta = 0.0035f;
    };

    inline float Clamp01(const float v)
    {
        return std::clamp(v, 0.0f, 1.0f);
    }

    inline float ComputeBT709Luma(const float r, const float g, const float b)
    {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    inline EdgeAdaptiveParams GetEdgeAdaptiveParams(const bool qdPanel)
    {
        return {
            qdPanel ? 0.40f : 0.50f,
            qdPanel ? 4.2f : 5.0f,
            qdPanel ? 0.65f : 0.75f,
            qdPanel ? 0.45f : 0.50f,
            qdPanel ? 0.65f : 0.60f,
            qdPanel ? 0.36f : 0.45f,
            qdPanel ? 0.20f : 0.25f,
            qdPanel ? 1.00f : 1.20f,
            qdPanel ? 0.35f : 0.45f
        };
    }

    inline float ComputeEdgeRisk(const float gradX,
                                 const float gradY,
                                 const float channelSpread,
                                 const float thinness,
                                 const EdgeAdaptiveParams& params)
    {
        const float verticalRisk =
            Clamp01((gradX - params.verticalGradBias * gradY) * params.verticalRiskGain);
        return Clamp01(
            (verticalRisk * params.edgeRiskVerticalW + channelSpread * params.edgeRiskSpreadW) *
            (params.edgeRiskThinLow + (1.0f - params.edgeRiskThinLow) * thinness));
    }

    inline float ComputeAdaptiveChromaKeep(const float chromaKeepBase,
                                           const float contrastSignal,
                                           const float edgeRisk,
                                           const EdgeAdaptiveParams& params)
    {
        const float safeContrast = Clamp01(contrastSignal);
        return Clamp01(
            chromaKeepBase * (0.7f + 0.3f * safeContrast) *
            (1.0f - params.chromaEdgeAtten * edgeRisk));
    }

    inline std::array<float, 3> SolveConstrainedLumaChroma(const std::array<float, 3>& m,
                                                            const float targetY)
    {
        constexpr std::array<float, 3> kW = {0.2126f, 0.7152f, 0.0722f};

        std::array<float, 3> x = m;
        std::array<bool, 3> isFree = {true, true, true};

        for (int iter = 0; iter < 4; ++iter)
        {
            float numerator = -targetY;
            float denominator = 0.0f;

            for (int i = 0; i < 3; ++i)
            {
                if (isFree[i])
                {
                    numerator += kW[i] * m[i];
                    denominator += kW[i] * kW[i];
                }
                else
                {
                    numerator += kW[i] * x[i];
                }
            }

            if (denominator <= 1e-8f)
            {
                break;
            }

            const float lambda = numerator / denominator;
            bool clampedAny = false;

            for (int i = 0; i < 3; ++i)
            {
                if (!isFree[i]) continue;

                const float v = m[i] - lambda * kW[i];
                if (v < 0.0f)
                {
                    x[i] = 0.0f;
                    isFree[i] = false;
                    clampedAny = true;
                }
                else if (v > 1.0f)
                {
                    x[i] = 1.0f;
                    isFree[i] = false;
                    clampedAny = true;
                }
                else
                {
                    x[i] = v;
                }
            }

            if (!clampedAny)
            {
                break;
            }
        }

        x[0] = Clamp01(x[0]);
        x[1] = Clamp01(x[1]);
        x[2] = Clamp01(x[2]);
        return x;
    }

    inline std::array<float, 3> ApplyConstrainedChromaOptimization(
        const std::array<float, 3>& cov,
        const float targetY,
        const float edgeRisk,
        const float contrastSignal,
        const float channelSpread,
        const EdgeAdaptiveParams& params,
        const ConstrainedChromaFastPath& fastPath = {})
    {
        const std::array<float, 3> clamped = {
            Clamp01(cov[0]),
            Clamp01(cov[1]),
            Clamp01(cov[2])
        };
        const float target = Clamp01(targetY);
        const float safeEdgeRisk = Clamp01(edgeRisk);
        const float safeContrast = Clamp01(contrastSignal);
        const float safeChannelSpread = Clamp01(channelSpread);
        const float currentY = ComputeBT709Luma(clamped[0], clamped[1], clamped[2]);

        if (fastPath.enabled &&
            safeEdgeRisk <= fastPath.maxEdgeRisk &&
            safeChannelSpread <= fastPath.maxChannelSpread &&
            std::abs(currentY - target) <= fastPath.maxLumaDelta)
        {
            return clamped;
        }

        const float chromaPenalty =
            params.chromaPenaltyBase +
            params.chromaPenaltyEdge * safeEdgeRisk +
            params.chromaPenaltyContrast * (1.0f - safeContrast);
        const float inv = 1.0f / (1.0f + chromaPenalty);

        const std::array<float, 3> unconstrained = {
            Clamp01((clamped[0] + chromaPenalty * target) * inv),
            Clamp01((clamped[1] + chromaPenalty * target) * inv),
            Clamp01((clamped[2] + chromaPenalty * target) * inv)
        };

        return SolveConstrainedLumaChroma(unconstrained, target);
    }
}
