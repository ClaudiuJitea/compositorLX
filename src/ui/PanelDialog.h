#pragma once
#include <QAbstractButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHash>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QWindow>

namespace compositor {
// The editor's own dialog window for messages, questions and short prompts: a rounded panel in the theme's colours (light
// or dark from the palette tokens), its buttons in the desktop's order (QDialogButtonBox follows KDE or GNOME), Escape
// for Cancel, and a drag anywhere on the panel to move it (startSystemMove also works on Wayland, where a frameless
// window cannot move itself).
class PanelDialog : public QDialog {
public:
    PanelDialog(QWidget *parent, const QString &title) : QDialog(parent)
    {
        setObjectName(QStringLiteral("messageDialog"));
        setWindowTitle(title);
        setAccessibleName(title);
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setModal(true);
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(10, 10, 10, 10);
        panel_ = new QWidget(this);
        panel_->setObjectName(QStringLiteral("messagePanel"));
        panel_->setAttribute(Qt::WA_StyledBackground);
        panel_->setMinimumWidth(440);
        content_ = new QVBoxLayout(panel_);
        content_->setContentsMargins(22, 20, 22, 18);
        content_->setSpacing(0);
        outer->addWidget(panel_);
    }
    [[nodiscard]] QWidget *panel() const { return panel_; }
    [[nodiscard]] QVBoxLayout *content() const { return content_; }
    static QLabel *makeLabel(QWidget *parent, const QString &text, const QString &name, Qt::TextFormat format = Qt::PlainText)
    {
        auto *label = new QLabel(text, parent);
        label->setObjectName(name);
        label->setWordWrap(true);
        label->setTextFormat(format);
        if (format == Qt::RichText) {
            label->setOpenExternalLinks(true);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        } else label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    }
    QLabel *addLabel(const QString &text, const QString &name, int spacingBefore, Qt::TextFormat format = Qt::PlainText)
    {
        if (spacingBefore > 0) content_->addSpacing(spacingBefore);
        QLabel *label = makeLabel(panel_, text, name, format);
        content_->addWidget(label);
        return label;
    }
    // Buttons that end the dialog with their standard button as the result; the primary one is the accent default.
    QDialogButtonBox *addButtons(QDialogButtonBox::StandardButtons buttons, QDialogButtonBox::StandardButton primary,
                                 const QHash<int, QString> &labels = {})
    {
        content_->addSpacing(20);
        auto *box = new QDialogButtonBox(buttons, panel_);
        box->setObjectName(QStringLiteral("messageButtons"));
        for (QAbstractButton *button : box->buttons()) {
            const QDialogButtonBox::StandardButton standard = box->standardButton(button);
            if (standard == QDialogButtonBox::Discard) { button->setText(QObject::tr("Don't Save")); button->setProperty("dialogRole", QStringLiteral("destructive")); }
            if (standard == primary) button->setProperty("dialogRole", QStringLiteral("primary"));
            if (labels.contains(int(standard))) button->setText(labels.value(int(standard)));
            button->setMinimumWidth(92);
            if (auto *push = qobject_cast<QPushButton *>(button)) {
                push->setAutoDefault(false);
                if (standard == primary) { push->setDefault(true); push->setFocus(); }
            }
            button->style()->unpolish(button); button->style()->polish(button);   // the role is set after the box made it
            QObject::connect(button, &QAbstractButton::clicked, this, [this, standard] { done(int(standard)); });
        }
        content_->addWidget(box);
        return box;
    }
protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && windowHandle() && windowHandle()->startSystemMove()) { event->accept(); return; }
        QDialog::mousePressEvent(event);
    }
private:
    QWidget *panel_ = nullptr;
    QVBoxLayout *content_ = nullptr;
};

} // namespace compositor
