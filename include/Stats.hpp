#pragma once
#include <vector>
#include <algorithm>
#include <cstddef>

struct LatencyStats {
    double min, p50, p99, p999, max, avg;
};

inline LatencyStats computeLatencyStats(std::vector<double> samplesMicros) {
    LatencyStats s{};
    if (samplesMicros.empty()) return s;
    std::sort(samplesMicros.begin(), samplesMicros.end());
    auto pct = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * (samplesMicros.size() - 1));
        return samplesMicros[idx];
    };
    s.min = samplesMicros.front();
    s.max = samplesMicros.back();
    s.p50 = pct(0.50);
    s.p99 = pct(0.99);
    s.p999 = pct(0.999);
    double sum = 0;
    for (double v : samplesMicros) sum += v;
    s.avg = sum / samplesMicros.size();
    return s;
}
