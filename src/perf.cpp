// =============================================================================
//  perf.cpp - rolling per-request timings exposed by GET /perf
// =============================================================================
#include "ttsc.h"

#include <algorithm>
#include <sstream>

namespace ttsc {
namespace {

struct Agg {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    void percentiles(double* p50, double* p95, double* mx) const {
        if (v.empty()) { *p50 = *p95 = *mx = 0; return; }
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        auto at = [&](double q) {
            const size_t i = size_t(q * double(s.size() - 1) + 0.5);
            return s[std::min(i, s.size() - 1)];
        };
        *p50 = at(0.50);
        *p95 = at(0.95);
        *mx  = s.back();
    }
    std::string json(const char* name) const {
        double a, b, c;
        percentiles(&a, &b, &c);
        std::ostringstream os;
        os << "\"" << name << "\":{\"n\":" << v.size() << ",\"p50\":" << a
           << ",\"p95\":" << b << ",\"max\":" << c << "}";
        return os.str();
    }
};

}  // namespace

void Perf::record(const Sample& s) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ring_.size() < kCapacity) ring_.push_back(s);
    else { ring_[next_] = s; next_ = (next_ + 1) % kCapacity; }
}

void Perf::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    ring_.clear();
    next_ = 0;
}

std::string Perf::json_summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    Agg total, first, rtf;
    size_t ok = 0, failed = 0;
    for (const auto& s : ring_) {
        total.add(s.total_ms);
        if (s.first_ms > 0) first.add(s.first_ms);
        if (s.audio_sec > 0 && s.total_ms > 0) rtf.add((s.total_ms / 1000.0) / s.audio_sec);
        if (s.ok) ++ok; else ++failed;
    }
    std::ostringstream os;
    os << "{" << total.json("total_ms") << "," << first.json("first_ms") << ","
       << rtf.json("rtf") << ",\"requests\":" << ring_.size() << ",\"ok\":" << ok
       << ",\"failed\":" << failed << "}";
    return os.str();
}

}  // namespace ttsc
