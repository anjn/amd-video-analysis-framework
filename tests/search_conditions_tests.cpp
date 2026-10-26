#include "search_conditions.hpp"
#include <QApplication>
#include <QEventLoop>
#include <iostream>
#include <stdexcept>
using namespace amdva::nvr;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        SearchConditions c;
        int calls = 0, invalidations = 0;
        bool clear = false;
        QString last;
        c.invalidate = [&](bool value) {
            ++invalidations;
            clear = value;
        };
        c.submit = [&] {
            ++calls;
            last = c.text();
        };
        c.input->setText("red");
        wait(150);
        c.input->setText("blue shirt");
        wait(250);
        require(calls == 0 && invalidations == 2, "Debounce cancels obsolete input immediately");
        wait(180);
        require(calls == 1 && last == "blue shirt", "Only latest text is submitted");
        c.input->setText("green shirt");
        QMetaObject::invokeMethod(c.input, "returnPressed", Qt::DirectConnection);
        require(calls == 2 && last == "green shirt", "Enter searches immediately");
        wait(420);
        require(calls == 2, "Enter does not also debounce");
        c.input->setText(" ");
        require(clear && !c.debounce.isActive() && calls == 2,
                "Empty input clears without inference");
        c.input->clear();
        QInputMethodEvent preedit(QString::fromUtf8("あか"), {});
        QApplication::sendEvent(c.input, &preedit);
        wait(420);
        require(calls == 2, "IME composition must not submit");
        QInputMethodEvent commit;
        commit.setCommitString(QString::fromUtf8("赤い服"));
        QApplication::sendEvent(c.input, &commit);
        wait(420);
        require(calls == 3 && last == QString::fromUtf8("赤い服"), "IME commit searches");
        c.input->setText("stale");
        c.mode->setCurrentIndex(SearchConditions::Person);
        wait(420);
        require(calls == 3 && clear, "Mode change cancels text debounce and clears results");
        auto check = [&](const char *name) {
            auto *box = c.findChild<QCheckBox *>(name);
            require(box, "Missing person checkbox");
            box->click();
        };
        check("search-person-upper-color-3");
        require(calls == 4 && last.contains("red upper-body clothing"),
                "Person selection is immediate");
        check("search-person-upper-color-4");
        require(calls == 5 && !last.contains("red") && last.contains("blue"), "Exclusive category");
        check("search-person-upper-0");
        check("search-person-age-1");
        require(last == "A young adult, wearing blue T-shirt.", "Stable combined description");
        check("search-person-upper-color-4");
        require(!last.contains("blue") && last.contains("T-shirt"),
                "Selected checkbox can be unset");
        c.findChild<QPushButton *>("search-person-clear")->click();
        require(c.person_text().isEmpty() && clear, "Clear resets all attributes");
        int before = calls;
        c.mode->setCurrentIndex(SearchConditions::Image);
        require(calls == before && c.empty(), "Empty image mode gives no query");
        c.image = {1, 1, {0, 0, 0}};
        c.changed(true, true);
        require(calls == before + 1, "Region image submits immediately");
        c.mode->setCurrentIndex(SearchConditions::Text);
        require(last == "stale", "Text conditions survive mode changes");
        c.input->setText("leaving");
        c.debounce.stop();
        before = calls;
        wait(420);
        require(calls == before, "Leaving cancels pending debounce");
        std::cout << "Search modes, debounce, IME and person conditions passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
