#include "amdva/streaming/person_tracking.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
namespace amdva::streaming {
float reid_similarity(const ReIdFeature &a, const ReIdFeature &b) {
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.f);
}
namespace {
float iou(const Detection &a, const Detection &b) {
    float w = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    float h = std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    return w * h / std::max(1.f, a.w * a.h + b.w * b.h - w * h);
}
void normalize(ReIdFeature &f) {
    float n = std::sqrt(std::max(1e-12f, reid_similarity(f, f)));
    for (auto &x : f)
        x /= n;
}
// Rectangular Hungarian assignment with explicit unmatched columns.
std::vector<int> assign(const std::vector<std::vector<double>> &costs, double limit) {
    const int n = costs.size(), real = n ? costs[0].size() : 0, m = real + n;
    std::vector<double> u(n + 1), v(m + 1);
    std::vector<int> p(m + 1), way(m + 1), out(n, -1);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> best(m + 1, 1e9);
        std::vector<bool> used(m + 1);
        do {
            used[j0] = true;
            int i0 = p[j0], j1 = 0;
            double delta = 1e9;
            for (int j = 1; j <= m; ++j)
                if (!used[j]) {
                    double c = (j <= real ? costs[i0 - 1][j - 1] : limit) - u[i0] - v[j];
                    if (c < best[j]) {
                        best[j] = c;
                        way[j] = j0;
                    }
                    if (best[j] < delta) {
                        delta = best[j];
                        j1 = j;
                    }
                }
            for (int j = 0; j <= m; ++j)
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else
                    best[j] -= delta;
            j0 = j1;
        } while (p[j0]);
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    for (int j = 1; j <= real; ++j)
        if (p[j] && costs[p[j] - 1][j - 1] < limit)
            out[p[j] - 1] = j - 1;
    return out;
}
// Four independent position/velocity Kalman filters on center x/y and width/height.
// Continuous timestamps keep motion meaningful when inference frames are skipped.
struct Motion {
    double x[4]{}, vel[4]{}, p[4][4]{};
    void init(Detection b) {
        double z[] = {b.x + b.w / 2, b.y + b.h / 2, b.w, b.h};
        for (int k = 0; k < 4; ++k) {
            x[k] = z[k];
            vel[k] = 0;
            p[k][0] = 100;
            p[k][1] = p[k][2] = 0;
            p[k][3] = 400;
        }
    }
    void predict(double dt) {
        for (int k = 0; k < 4; ++k) {
            x[k] += dt * vel[k];
            auto *c = p[k];
            double a = c[0] + dt * (c[1] + c[2]) + dt * dt * c[3];
            c[1] += dt * c[3];
            c[2] += dt * c[3];
            double q = std::max(1., x[k < 2 ? k + 2 : k] * .05);
            c[0] = a + q * q * dt * dt;
            c[3] += q * q * dt;
        }
    }
    void correct(Detection b) {
        double z[] = {b.x + b.w / 2, b.y + b.h / 2, b.w, b.h};
        for (int k = 0; k < 4; ++k) {
            auto *c = p[k];
            double r = std::max(1., z[k < 2 ? k + 2 : k] * .05);
            r *= r;
            double a = c[0], b1 = c[1], c1 = c[2], k0 = a / (a + r), k1 = c1 / (a + r),
                   e = z[k] - x[k];
            x[k] += k0 * e;
            vel[k] += k1 * e;
            c[0] -= k0 * a;
            c[1] -= k0 * b1;
            c[2] -= k1 * a;
            c[3] -= k1 * b1;
        }
    }
    Detection box() const {
        return {float(x[0] - x[2] / 2),
                float(x[1] - x[3] / 2),
                float(std::max(1., x[2])),
                float(std::max(1., x[3])),
                1,
                0};
    }
};
} // namespace
struct PersonTracker::Impl {
    struct Track {
        uint64_t id{}, person{};
        int64_t last{}, predicted{};
        unsigned hits{};
        Motion motion;
        Detection last_box{};
        ReIdFeature feature{};
        std::deque<ReIdFeature> gallery;
    };
    std::vector<Track> tracks;
    uint64_t next_track{}, next_person{};
    int64_t last_time{};
};
PersonTracker::PersonTracker() : impl_(std::make_unique<Impl>()) {}
PersonTracker::~PersonTracker() = default;
std::vector<PersonIdentity> PersonTracker::update(int64_t now,
                                                  const std::vector<PersonDetection> &detections) {
    auto &s = *impl_;
    std::vector<PersonIdentity> out(detections.size());
    if (now <= s.last_time)
        return out;
    s.last_time = now;
    auto &ts = s.tracks;
    ts.erase(std::remove_if(ts.begin(), ts.end(),
                            [&](auto &t) {
                                return now - t.last > 30000 || (t.hits < 2 && now - t.last > 1000);
                            }),
             ts.end());
    std::vector<bool> used(detections.size()), matched(ts.size());
    for (auto &t : ts) {
        t.motion.predict(std::min(2., (now - t.predicted) / 1000.));
        t.predicted = now;
    }
    auto accept = [&](size_t i, size_t j) {
        auto &t = ts[i];
        const auto &d = detections[j];
        t.motion.correct(d.box);
        t.last_box = d.box;
        t.last = now;
        ++t.hits;
        if (d.box.score >= .5f && reid_similarity(d.feature, d.feature) > .5f) {
            if (t.gallery.empty())
                t.feature = d.feature;
            else
                for (size_t k = 0; k < 512; ++k)
                    t.feature[k] = .9f * t.feature[k] + .1f * d.feature[k];
            normalize(t.feature);
            t.gallery.push_back(d.feature);
            if (t.gallery.size() > 5)
                t.gallery.pop_front();
        }
        out[j] = {t.id, t.person, t.hits >= 2};
        used[j] = true;
        matched[i] = true;
    };
    // High-score motion/appearance association, then low-score IoU recovery.
    for (int stage = 0; stage < 2; ++stage) {
        std::vector<size_t> rows, cols;
        for (size_t i = 0; i < ts.size(); ++i)
            if (!matched[i] && now - ts[i].last <= 2000 && (stage == 0 || now - ts[i].last <= 1000))
                rows.push_back(i);
        for (size_t j = 0; j < detections.size(); ++j)
            if (!used[j] &&
                (stage == 0 ? detections[j].box.score >= .5f
                            : detections[j].box.score >= .1f && detections[j].box.score < .5f))
                cols.push_back(j);
        std::vector<std::vector<double>> cost(rows.size(), std::vector<double>(cols.size(), 1));
        for (size_t r = 0; r < rows.size(); ++r)
            for (size_t c = 0; c < cols.size(); ++c) {
                auto &t = ts[rows[r]];
                auto &d = detections[cols[c]];
                float overlap = iou(t.motion.box(), d.box);
                double v = 1 - overlap;
                if (stage == 0) {
                    v = 1 - overlap * d.box.score;
                    float sim = reid_similarity(t.feature, d.feature);
                    // Strong appearance disagreement rejects a geometrically close distractor.
                    if (!t.gallery.empty() && sim < .45f) {
                        cost[r][c] = 1;
                        continue;
                    }
                    if (overlap >= .1f && sim >= .75f)
                        v = std::min(v, double(1 - sim));
                }
                cost[r][c] = v;
            }
        auto pairs = assign(cost, stage == 0 ? .8 : .5);
        for (size_t r = 0; r < pairs.size(); ++r)
            if (pairs[r] >= 0)
                accept(rows[r], cols[pairs[r]]);
    }
    // Reappearance requires mutually unique appearance and a plausible nearby location.
    // Never merge overlapping active tracks. Ambiguous candidates remain separate.
    for (size_t j = 0; j < detections.size(); ++j)
        if (!used[j] && detections[j].box.score >= .6f) {
            auto &d = detections[j];
            int best = -1;
            float top = -1, second = -1;
            for (size_t i = 0; i < ts.size(); ++i)
                if (!matched[i] && ts[i].hits >= 2 && now - ts[i].last > 2000) {
                    auto &t = ts[i];
                    float dx = (d.box.x + d.box.w / 2) - (t.last_box.x + t.last_box.w / 2),
                          dy = (d.box.y + d.box.h / 2) - (t.last_box.y + t.last_box.h / 2);
                    if (std::hypot(dx, dy) > 3 * std::max(d.box.h, t.last_box.h))
                        continue;
                    float sim = reid_similarity(t.feature, d.feature);
                    if (sim > top) {
                        second = top;
                        top = sim;
                        best = i;
                    } else
                        second = std::max(second, sim);
                }
            if (best >= 0 && top >= .85f && top - second >= .05f) {
                float competitor = -1;
                for (size_t k = 0; k < detections.size(); ++k)
                    if (k != j && !used[k])
                        competitor = std::max(
                            competitor, reid_similarity(ts[best].feature, detections[k].feature));
                if (top - competitor >= .05f) {
                    ts[best].id = ++s.next_track;
                    ts[best].motion.init(d.box);
                    accept(best, j);
                    continue;
                }
            }
            Impl::Track t;
            t.id = ++s.next_track;
            t.person = ++s.next_person;
            t.last = t.predicted = now;
            t.hits = 1;
            t.last_box = d.box;
            t.motion.init(d.box);
            t.feature = d.feature;
            if (reid_similarity(d.feature, d.feature) > .5f)
                t.gallery.push_back(d.feature);
            ts.push_back(t);
            matched.push_back(true);
            out[j] = {t.id, t.person, false};
        }
    // Memory is bounded even under adversarial detections.
    if (ts.size() > 512)
        ts.erase(ts.begin(), ts.begin() + (ts.size() - 512));
    return out;
}
} // namespace amdva::streaming
