#pragma once
#include <QPainterPath>
#include <QRawFont>
#include <QString>
#include <map>
#include <vector>
namespace amdva::nvr {
// Tessellated outline geometry only: no bitmap glyphs or CPU texture uploads.
std::vector<QPointF> triangulate_text_path(const QPainterPath &);
class VectorText {
  public:
    struct Run {
        std::vector<QPointF> triangles;
        double width{}, ascent{}, height{};
    };
    Run layout(const QString &);

  private:
    struct Glyph {
        std::vector<QPointF> triangles;
        double advance{}, ascent{}, descent{};
    };
    QRawFont font_;
    std::map<char32_t, Glyph> glyphs_;
};
} // namespace amdva::nvr
