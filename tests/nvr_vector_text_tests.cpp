#include "vector_text.hpp"
#include <QFontDatabase>
#include <QGuiApplication>
#include <cmath>
#include <iostream>
using namespace amdva::nvr;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
double area(const std::vector<QPointF> &triangles) {
    double result = 0;
    for (size_t i = 0; i < triangles.size(); i += 3) {
        auto a = triangles[i], b = triangles[i + 1], c = triangles[i + 2];
        result +=
            std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
    }
    return result;
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    try {
        QPainterPath hole;
        hole.setFillRule(Qt::OddEvenFill);
        hole.addRect(0, 0, 10, 10);
        hole.addRect(3, 3, 4, 4);
        check(std::abs(area(triangulate_text_path(hole)) - 84) < 1e-6,
              "Glyph hole filled incorrectly");
        QPainterPath overlapping;
        overlapping.setFillRule(Qt::WindingFill);
        overlapping.addRect(0, 0, 6, 10);
        overlapping.addRect(4, 0, 6, 10);
        check(std::abs(area(triangulate_text_path(overlapping)) - 100) < 1e-6,
              "Overlapping contours double filled");
        VectorText font;
        auto run = font.layout("H.264 1920x1080 29.7 fps 5.5 Mbps");
        check(run.width > 0 && run.height > 0 && !run.triangles.empty(), "Font outline missing");
        auto same = font.layout("H.264 1920x1080 29.7 fps 5.5 Mbps");
        check(same.triangles == run.triangles && same.width == run.width,
              "Cached outlines changed");
        const auto camera = font.layout(QString::fromUtf8("入口カメラ 𠮷 Live"));
        check(camera.width > 0 && !camera.triangles.empty(), "Unicode camera name outline missing");
        check(font.layout(QString::fromUtf8("入口カメラ 𠮷 Live")).triangles == camera.triangles,
              "Fallback font cache changed");
        const auto families = QFontDatabase::families();
        if (families.contains("Noto Sans CJK JP") || families.contains("Noto Serif CJK JP"))
            check(font.layout(QString::fromUtf8("入")).triangles !=
                      font.layout(QString(QChar::ReplacementCharacter)).triangles,
                  "Available Japanese glyph replaced by placeholder");
        auto spaces = font.layout("   ");
        check(spaces.width > 0 && spaces.triangles.empty(), "Space rendered a glyph");
        for (auto point : run.triangles)
            check(std::isfinite(point.x()) && std::isfinite(point.y()),
                  "Non-finite glyph geometry");
        std::cout << "Outlined text, holes, overlaps and cache passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
