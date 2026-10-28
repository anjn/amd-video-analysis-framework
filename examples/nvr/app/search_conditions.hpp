#pragma once
#include "backend/search.hpp"
#include "flow_layout.hpp"
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
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
    QButtonGroup *mode{};
    SearchTextInput *input{};
    QLabel *preview{}, *description{};
    QTimer debounce;
    SearchImage image;
    std::function<void(bool)> invalidate;
    std::function<void()> submit;
    // Button id 0 is "any"; id i + 1 selects words[i].
    struct Attribute {
        QString id, title;
        QStringList words;
        bool swatches{};
        QButtonGroup *group{};
        QAbstractButton *any{};
        std::vector<QAbstractButton *> checks;
    };
    std::vector<Attribute> attributes;
    explicit SearchConditions(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("search-conditions");
        auto *layout = new QVBoxLayout(this);
        auto *modes = new QWidget;
        modes->setObjectName("search-mode");
        modes->setStyleSheet("QPushButton{padding:5px}"
                             "QPushButton:checked{background:palette(highlight);"
                             "color:palette(highlighted-text);border:1px solid palette(dark)}");
        auto *ml = new QHBoxLayout(modes);
        ml->setContentsMargins(0, 0, 0, 0);
        ml->setSpacing(0);
        mode = new QButtonGroup(this);
        const char *names[] = {"image", "text", "person"};
        const char *titles[] = {"Image", "Text", "Person"};
        for (int i = Image; i <= Person; ++i) {
            auto *button = new QPushButton(titles[i]);
            button->setObjectName(QString("search-mode-%1").arg(names[i]));
            button->setCheckable(true);
            mode->addButton(button, i);
            ml->addWidget(button);
        }
        mode->button(Person)->setChecked(true);
        layout->addWidget(modes);
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
        pl->setSpacing(6);
        description = new QLabel;
        description->setObjectName("search-person-description");
        description->setTextFormat(Qt::PlainText);
        description->setWordWrap(true);
        description->setFrameShape(QFrame::StyledPanel);
        description->setMargin(8);
        auto description_font = description->font();
        description_font.setPointSizeF(description_font.pointSizeF() * 1.25);
        description->setFont(description_font);
        pl->addWidget(description);
        pl->addSpacing(8);
        auto *clear = new QPushButton("Clear conditions");
        clear->setObjectName("search-person-clear");
        pl->addWidget(clear);
        pl->addSpacing(12);
        // "none" comes first so that it is displayed right after "any".
        attributes = {
            {"gender", "Gender", {"man", "woman"}},
            {"age", "Age", {"teenager", "young adult", "middle-aged adult", "older adult"}},
            {"build", "Build", {"slim", "average", "stocky"}},
            {"skin", "Skin tone", {"light", "medium", "dark"}},
            {"hair",
             "Hair color",
             {"black", "dark brown", "brown", "ash brown", "gray-brown", "dark gray", "gray",
              "silver"}},
            {"hair-length", "Hair length", {"short", "medium-length", "long"}},
            {"upper",
             "Upper",
             {"T-shirt", "cropped T-shirt", "collared shirt", "collared knit top", "jacket",
              "quilted jacket"}},
            {"lower", "Lower", {"trousers", "shorts", "leggings", "skirt"}},
            {"upper-color", "Upper color", colors(), true},
            {"lower-color", "Lower color", colors(), true},
            {"glasses", "Eyewear", {"none", "eyeglasses"}},
            {"hat",
             "Hat",
             {"none", "beanie", "knitted hat", "flat cap", "fedora", "cloche hat", "fishing hat"}},
            {"bag",
             "Bag",
             {"none", "backpack", "shoulder bag", "sling bag", "handbag", "paper shopping bag"}}};
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(12);
        grid->setColumnStretch(1, 1);
        for (size_t a = 0; a < attributes.size(); ++a) {
            auto &attribute = attributes[a];
            auto *title = new QLabel(attribute.title);
            auto font = title->font();
            font.setBold(true);
            title->setFont(font);
            grid->addWidget(title, int(a), 0, Qt::AlignTop | Qt::AlignLeft);
            auto *choices = new QWidget;
            auto *flow = new FlowLayout(choices, 4);
            attribute.group = new QButtonGroup(this);
            attribute.any = new QRadioButton("any");
            attribute.any->setObjectName(QString("search-person-%1-any").arg(attribute.id));
            attribute.any->setChecked(true);
            attribute.group->addButton(attribute.any, 0);
            flow->addWidget(attribute.any);
            for (int i = 0; i < attribute.words.size(); ++i) {
                QAbstractButton *button;
                if (attribute.swatches) {
                    auto *swatch = new QToolButton;
                    swatch->setCheckable(true);
                    swatch->setFixedSize(22, 22);
                    swatch->setToolTip(attribute.words[i]);
                    swatch->setStyleSheet(
                        QString("QToolButton{background:%1;border:1px solid #808080;"
                                "border-radius:3px}"
                                "QToolButton:checked{border:3px solid palette(highlight)}")
                            .arg(color_value(attribute.words[i])));
                    button = swatch;
                } else {
                    button = new QRadioButton(attribute.words[i]);
                    // Highlight chosen values; "any" stays plain. Style sheets cannot
                    // change the font per state, so boldness follows the toggle.
                    button->setStyleSheet("QRadioButton:checked{color:#1565c0}");
                    connect(button, &QAbstractButton::toggled, button, [button](bool on) {
                        auto font = button->font();
                        font.setBold(on);
                        button->setFont(font);
                    });
                }
                button->setObjectName(QString("search-person-%1-%2").arg(attribute.id).arg(i));
                // Choosing the selected value again returns the condition to "any".
                connect(button, &QAbstractButton::pressed, button,
                        [button] { button->setProperty("was-checked", button->isChecked()); });
                connect(button, &QAbstractButton::clicked, this, [button, any = attribute.any] {
                    if (button->property("was-checked").toBool())
                        any->setChecked(true);
                });
                attribute.checks.push_back(button);
                attribute.group->addButton(button, i + 1);
                flow->addWidget(button);
            }
            grid->addWidget(choices, int(a), 1);
            connect(attribute.group, &QButtonGroup::idToggled, this, [this](int, bool checked) {
                if (!checked)
                    return;
                refresh_description();
                if (current_mode() == Person)
                    changed(false, true);
            });
        }
        pl->addLayout(grid);
        pl->addStretch();
        stack->addWidget(person_page);
        stack->setCurrentIndex(Person);
        // Do not let the tall inactive person page determine the other modes' height.
        auto resize_pages = [stack](int current) {
            for (int i = 0; i < stack->count(); ++i)
                stack->widget(i)->setSizePolicy(QSizePolicy::Preferred, i == current
                                                                            ? QSizePolicy::Preferred
                                                                            : QSizePolicy::Ignored);
            stack->adjustSize();
        };
        resize_pages(Person);
        connect(mode, &QButtonGroup::idToggled, this,
                [this, stack, resize_pages](int index, bool checked) {
                    if (!checked)
                        return;
                    stack->setCurrentIndex(index);
                    resize_pages(index);
                    changed(true, true);
                });
        debounce.setSingleShot(true);
        debounce.setInterval(350);
        connect(&debounce, &QTimer::timeout, this, [this] {
            if (submit && current_mode() == Text && !input->composing)
                submit();
        });
        auto edited = [this] {
            if (current_mode() == Text)
                changed(false, false);
        };
        connect(input, &QLineEdit::textChanged, this, [edited](const QString &) { edited(); });
        input->composition_changed = edited;
        connect(input, &QLineEdit::returnPressed, this, [this] {
            if (current_mode() == Text && !input->composing)
                changed(false, true);
        });
        connect(clear, &QPushButton::clicked, this, [this] {
            for (auto &a : attributes) {
                QSignalBlocker block(a.group);
                a.any->setChecked(true);
            }
            refresh_description();
            changed(false, true);
        });
        refresh_description();
    }
    int current_mode() const { return mode->checkedId(); }
    void set_mode(Mode m) { mode->button(m)->setChecked(true); }
    static QStringList colors() {
        return {"black",  "white", "gray",  "red",  "blue",   "green",
                "yellow", "brown", "beige", "pink", "purple", "orange"};
    }
    static QString color_value(const QString &name) {
        static const QStringList values = {"#000000", "#ffffff", "#808080", "#d32f2f",
                                           "#1e5bd8", "#2e8b3d", "#f2d21b", "#7b4a26",
                                           "#e8d8b0", "#f4a6c1", "#7e3fa0", "#f08a1c"};
        return values.value(colors().indexOf(name));
    }
    QString value(const QString &id) const {
        for (const auto &a : attributes)
            if (a.id == id) {
                const int checked = a.group->checkedId();
                return checked > 0 ? a.words[checked - 1] : QString();
            }
        return {};
    }
    static QString article(const QString &word) {
        return QString("aeiou").contains(word.front()) ? "an " : "a ";
    }
    QString person_text() const {
        if (std::all_of(attributes.begin(), attributes.end(),
                        [](const auto &a) { return a.group->checkedId() <= 0; }))
            return {};
        const auto gender = value("gender"), age = value("age");
        QString subject;
        if (gender.isEmpty())
            subject = age.isEmpty() ? "person" : age;
        else if (age == "teenager")
            subject = gender == "man" ? "teenage boy" : "teenage girl";
        else
            subject = age.isEmpty() ? gender : QString(age).remove(" adult") + " " + gender;
        QString text = article(subject) + subject;
        text[0] = text[0].toUpper();
        QStringList details;
        const auto build = value("build"), skin = value("skin"), hair = value("hair"),
                   length = value("hair-length"), upper = value("upper"), lower = value("lower"),
                   upper_color = value("upper-color"), lower_color = value("lower-color"),
                   glasses = value("glasses"), hat = value("hat"), bag = value("bag");
        if (!build.isEmpty())
            details << "with " + article(build) + build + " build";
        if (!skin.isEmpty())
            details << "with " + skin + " skin";
        if (!hair.isEmpty() || !length.isEmpty())
            details << "with " + (length.isEmpty() ? "" : length + " ") +
                           (hair.isEmpty() ? "" : hair + " ") + "hair";
        if (!upper.isEmpty() || !upper_color.isEmpty())
            details << "wearing " + (upper_color.isEmpty() ? "" : upper_color + " ") +
                           (upper.isEmpty() ? "upper-body clothing" : upper);
        if (!lower.isEmpty() || !lower_color.isEmpty())
            details << "wearing " + (lower_color.isEmpty() ? "" : lower_color + " ") +
                           (lower.isEmpty() ? "lower-body clothing" : lower);
        if (!glasses.isEmpty())
            details << (glasses == "none" ? "without glasses" : "wearing " + glasses);
        if (!hat.isEmpty())
            details << (hat == "none" ? "without a hat" : "wearing " + article(hat) + hat);
        if (!bag.isEmpty())
            details << (bag == "none" ? "without a bag" : "carrying a " + bag);
        if (!details.isEmpty())
            text += ", " + details.join(", ");
        return text + ".";
    }
    QString text() const {
        return current_mode() == Person ? person_text() : input->text().trimmed();
    }
    bool empty() const { return current_mode() == Image ? image.rgb.empty() : text().isEmpty(); }
    void changed(bool clear, bool immediate) {
        debounce.stop();
        if (invalidate)
            invalidate(clear || empty());
        if (empty())
            return;
        if (current_mode() == Text && input->composing)
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
