#pragma once
#include "amdva/streaming/search.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
namespace amdva::nvr {
class SearchTextInput : public QLineEdit {
  public:
    bool composing{};
    std::function<void()> composition_changed;

  protected:
    void inputMethodEvent(QInputMethodEvent *event) override {
        composing = !event->preeditString().isEmpty();
        QLineEdit::inputMethodEvent(event);
        if (composition_changed)
            composition_changed();
    }
};
// UI-only query state; no inference, catalog access or persistent settings.
class SearchConditions : public QWidget {
  public:
    enum Mode { Image, Text, Person };
    QComboBox *mode{};
    SearchTextInput *input{};
    QLabel *preview{}, *description{};
    QTimer debounce;
    streaming::SearchImage image;
    std::function<void(bool)> invalidate;
    std::function<void()> submit;
    struct Attribute {
        QString id, title;
        QStringList words;
        std::vector<QCheckBox *> checks;
    };
    std::vector<Attribute> attributes;
    explicit SearchConditions(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("search-conditions");
        auto *layout = new QVBoxLayout(this);
        mode = new QComboBox;
        mode->setObjectName("search-mode");
        mode->addItems({"Image search", "Text search", "Person search"});
        mode->setCurrentIndex(Text);
        layout->addWidget(mode);
        auto *stack = new QStackedWidget;
        layout->addWidget(stack);
        auto *image_page = new QWidget;
        auto *il = new QVBoxLayout(image_page);
        auto *hint = new QLabel("Select a region in live or recorded video to search by image.");
        hint->setWordWrap(true);
        il->addWidget(hint);
        preview = new QLabel;
        preview->setObjectName("search-query-image");
        il->addWidget(preview);
        il->addStretch();
        stack->addWidget(image_page);
        auto *text_page = new QWidget;
        auto *tl = new QVBoxLayout(text_page);
        input = new SearchTextInput;
        input->setObjectName("search-text");
        input->setPlaceholderText("Describe a person or clothing…");
        tl->addWidget(input);
        auto *text_hint =
            new QLabel("Search updates after you stop typing. Press Enter to search now.");
        text_hint->setWordWrap(true);
        tl->addWidget(text_hint);
        tl->addStretch();
        stack->addWidget(text_page);
        auto *person_page = new QWidget;
        auto *pl = new QVBoxLayout(person_page);
        auto *hint_person =
            new QLabel("Find people with a similar appearance. Attributes are not exact filters.");
        hint_person->setWordWrap(true);
        pl->addWidget(hint_person);
        auto *clear = new QPushButton("Clear conditions");
        clear->setObjectName("search-person-clear");
        pl->addWidget(clear);
        attributes = {
            {"age", "Age group", {"child", "young adult", "middle-aged adult", "older adult"}, {}},
            {"skin", "Skin tone", {"light", "medium", "dark"}, {}},
            {"hair", "Hair color", {"black", "brown", "blond", "red", "gray", "white"}, {}},
            {"upper",
             "Upper clothing",
             {"T-shirt", "shirt", "sweater", "hoodie", "jacket", "coat"},
             {}},
            {"lower", "Lower clothing", {"long pants", "shorts", "skirt"}, {}},
            {"upper-color", "Upper clothing color", colors(), {}},
            {"lower-color", "Lower clothing color", colors(), {}},
            {"glasses", "Eyewear", {"eyeglasses", "sunglasses", "none"}, {}},
            {"bag",
             "Bag",
             {"backpack", "shoulder bag", "handbag", "tote bag", "suitcase", "none"},
             {}}};
        for (size_t a = 0; a < attributes.size(); ++a) {
            auto &attribute = attributes[a];
            auto *box = new QGroupBox(attribute.title);
            auto *choices = new QGridLayout(box);
            for (int i = 0; i < attribute.words.size(); ++i) {
                auto *check = new QCheckBox(attribute.words[i]);
                check->setObjectName(QString("search-person-%1-%2").arg(attribute.id).arg(i));
                attribute.checks.push_back(check);
                choices->addWidget(check, i / 2, i % 2);
                connect(check, &QCheckBox::toggled, this, [this, a, check](bool checked) {
                    if (checked)
                        for (auto *other : attributes[a].checks)
                            if (other != check) {
                                QSignalBlocker block(other);
                                other->setChecked(false);
                            }
                    refresh_description();
                    if (mode->currentIndex() == Person)
                        changed(false, true);
                });
            }
            pl->addWidget(box);
        }
        description = new QLabel;
        description->setObjectName("search-person-description");
        description->setTextFormat(Qt::PlainText);
        description->setWordWrap(true);
        pl->insertWidget(2, description);
        pl->addStretch();
        stack->addWidget(person_page);
        stack->setCurrentIndex(Text);
        // Do not let the tall inactive person page determine the other modes' height.
        auto resize_pages = [stack](int current) {
            for (int i = 0; i < stack->count(); ++i)
                stack->widget(i)->setSizePolicy(QSizePolicy::Preferred, i == current
                                                                            ? QSizePolicy::Preferred
                                                                            : QSizePolicy::Ignored);
            stack->adjustSize();
        };
        resize_pages(Text);
        connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, stack, resize_pages](int index) {
                    stack->setCurrentIndex(index);
                    resize_pages(index);
                    changed(true, true);
                });
        debounce.setSingleShot(true);
        debounce.setInterval(350);
        connect(&debounce, &QTimer::timeout, this, [this] {
            if (submit && mode->currentIndex() == Text && !input->composing)
                submit();
        });
        auto edited = [this] {
            if (mode->currentIndex() == Text)
                changed(false, false);
        };
        connect(input, &QLineEdit::textChanged, this, [edited](const QString &) { edited(); });
        input->composition_changed = edited;
        connect(input, &QLineEdit::returnPressed, this, [this] {
            if (mode->currentIndex() == Text && !input->composing)
                changed(false, true);
        });
        connect(clear, &QPushButton::clicked, this, [this] {
            for (auto &a : attributes)
                for (auto *c : a.checks) {
                    QSignalBlocker block(c);
                    c->setChecked(false);
                }
            refresh_description();
            changed(false, true);
        });
        refresh_description();
    }
    static QStringList colors() {
        return {"black",  "white", "gray",  "red",  "blue",   "green",
                "yellow", "brown", "beige", "pink", "purple", "orange"};
    }
    QString person_text() const {
        QStringList selected;
        for (const auto &a : attributes) {
            QString value;
            for (size_t i = 0; i < a.checks.size(); ++i)
                if (a.checks[i]->isChecked())
                    value = a.words[int(i)];
            selected << value;
        }
        if (std::all_of(selected.begin(), selected.end(),
                        [](const auto &s) { return s.isEmpty(); }))
            return {};
        QString text = selected[0].isEmpty()
                           ? "A person"
                           : (selected[0] == "older adult" ? "An " : "A ") + selected[0];
        QStringList details;
        if (!selected[1].isEmpty())
            details << "with " + selected[1] + " skin";
        if (!selected[2].isEmpty())
            details << "with " + selected[2] + " hair";
        if (!selected[3].isEmpty() || !selected[5].isEmpty())
            details << "wearing " + (selected[5].isEmpty() ? "" : selected[5] + " ") +
                           (selected[3].isEmpty() ? "upper-body clothing" : selected[3]);
        if (!selected[4].isEmpty() || !selected[6].isEmpty())
            details << "wearing " + (selected[6].isEmpty() ? "" : selected[6] + " ") +
                           (selected[4].isEmpty() ? "lower-body clothing" : selected[4]);
        if (!selected[7].isEmpty())
            details << (selected[7] == "none" ? "without glasses" : "wearing " + selected[7]);
        if (!selected[8].isEmpty())
            details << (selected[8] == "none" ? "without a bag" : "carrying a " + selected[8]);
        if (!details.isEmpty())
            text += ", " + details.join(", ");
        return text + ".";
    }
    QString text() const {
        return mode->currentIndex() == Person ? person_text() : input->text().trimmed();
    }
    bool empty() const {
        return mode->currentIndex() == Image ? image.rgb.empty() : text().isEmpty();
    }
    void changed(bool clear, bool immediate) {
        debounce.stop();
        if (invalidate)
            invalidate(clear || empty());
        if (empty())
            return;
        if (mode->currentIndex() == Text && input->composing)
            return;
        if (immediate) {
            if (submit)
                submit();
        } else
            debounce.start();
    }
    void refresh_description() {
        description->setText(person_text().isEmpty() ? "Select appearance features."
                                                     : person_text());
    }
};
} // namespace amdva::nvr
