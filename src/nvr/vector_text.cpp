#include "vector_text.hpp"
#include <QFont>
#include <QGlyphRun>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace amdva::nvr {
std::vector<QPointF> triangulate_text_path(const QPainterPath &source) {
    // Simplification resolves overlapping contours and produces odd-even fill.
    // Bands between contour vertices contain non-crossing straight edges, so
    // pairs of intersections form trapezoids, including glyph holes.
    struct Edge {
        QPointF a, b;
        double x(double y) const { return a.x() + (y - a.y()) * (b.x() - a.x()) / (b.y() - a.y()); }
    };
    std::vector<Edge> edges;
    std::vector<double> levels;
    for (const auto &polygon : source.simplified().toSubpathPolygons()) {
        for (int i = 0; i < polygon.size(); ++i) {
            // Qt may return almost-identical contour levels a few ULPs apart.
            // Snap well below subpixel precision before constructing scan bands.
            auto snap = [](QPointF p) {
                return QPointF(std::round(p.x() * 1e6) / 1e6, std::round(p.y() * 1e6) / 1e6);
            };
            auto a = snap(polygon[i]), b = snap(polygon[(i + 1) % polygon.size()]);
            if (a.y() == b.y())
                continue;
            if (a.y() > b.y())
                std::swap(a, b);
            edges.push_back({a, b});
            levels.push_back(a.y());
            levels.push_back(b.y());
        }
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    std::vector<QPointF> triangles;
    for (size_t i = 1; i < levels.size(); ++i) {
        const double top = levels[i - 1], bottom = levels[i], middle = (top + bottom) / 2;
        std::vector<const Edge *> active;
        for (const auto &edge : edges)
            if (edge.a.y() < middle && middle < edge.b.y())
                active.push_back(&edge);
        std::sort(active.begin(), active.end(),
                  [&](auto a, auto b) { return a->x(middle) < b->x(middle); });
        if (active.size() % 2)
            throw std::runtime_error("Invalid font outline contours");
        for (size_t j = 0; j < active.size(); j += 2) {
            QPointF a(active[j]->x(top), top), b(active[j + 1]->x(top), top),
                c(active[j]->x(bottom), bottom), d(active[j + 1]->x(bottom), bottom);
            triangles.insert(triangles.end(), {a, b, c, c, b, d});
        }
    }
    return triangles;
}
VectorText::Run VectorText::layout(const QString &text) {
    if (!font_.isValid()) {
        QFont font("DejaVu Sans");
        font.setPixelSize(64);
        font.setWeight(QFont::DemiBold);
        font_ = QRawFont::fromFont(font);
        if (!font_.isValid())
            throw std::runtime_error("Outline font unavailable");
    }
    Run result;
    result.ascent = font_.ascent();
    result.height = font_.ascent() + font_.descent();
    for (char32_t codepoint : text.toUcs4()) {
        auto found = glyphs_.find(codepoint);
        if (found == glyphs_.end()) {
            const auto character = QString::fromUcs4(&codepoint, 1);
            auto face = font_;
            auto indexes = face.glyphIndexesForString(character);
            if (indexes.empty() || (!indexes[0] && !QChar::isSpace(codepoint))) {
                // Camera names may need CJK or other fallback faces. Qt resolves
                // the face, but we still render its outlines rather than bitmaps.
                QFont requested("DejaVu Sans");
                requested.setPixelSize(64);
                requested.setWeight(QFont::DemiBold);
                QTextLayout shaped(character, requested);
                shaped.beginLayout();
                auto line = shaped.createLine();
                if (line.isValid())
                    line.setLineWidth(10000);
                shaped.endLayout();
                const auto runs = shaped.glyphRuns();
                if (!runs.empty()) {
                    face = runs.front().rawFont();
                    indexes = runs.front().glyphIndexes();
                }
                if (indexes.empty() || !indexes[0] || !face.isValid()) {
                    face = font_;
                    indexes = face.glyphIndexesForString(QString(QChar::ReplacementCharacter));
                }
            }
            const auto advances = face.advancesForGlyphIndexes(indexes);
            Glyph glyph{triangulate_text_path(face.pathForGlyph(indexes[0])), advances[0].x(),
                        face.ascent(), face.descent()};
            found = glyphs_.emplace(codepoint, std::move(glyph)).first;
        }
        result.ascent = std::max(result.ascent, found->second.ascent);
        result.height = std::max(result.height, result.ascent + found->second.descent);
        for (auto point : found->second.triangles)
            result.triangles.push_back(point + QPointF(result.width, 0));
        result.width += found->second.advance;
    }
    return result;
}
} // namespace amdva::nvr
