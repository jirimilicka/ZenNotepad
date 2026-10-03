#pragma once
#include "editor.h"
#include "portal.h"
#include "search.h"
#include "translations.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QAbstractButton>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

// Clear button inside a line edit. Qt's built-in one only comes in two fixed icon sizes, so
// this one is drawn as a vector shape and scales with the font (Ctrl+wheel zoom).
class LineClearButton : public QAbstractButton {
public:
    explicit LineClearButton(QLineEdit *le) : QAbstractButton(le), le_(le)
    {
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
        setVisible(false);
        QObject::connect(this, &QAbstractButton::clicked, le, [le] {
            le->clear();
            le->setFocus();
        });
        QObject::connect(le, &QLineEdit::textChanged, this, [this](const QString &t) { setVisible(!t.isEmpty()); });
        le->installEventFilter(this);
        relayout();
    }

protected:
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::Resize || e->type() == QEvent::FontChange)
            relayout();
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor c = palette().color(QPalette::Text);
        c.setAlphaF(underMouse() ? 0.85 : 0.5);
        // backspace key shape with an x inside
        const qreal w = width(), h = height(), m = h * 0.2;
        const qreal top = m, bot = h - m, left = w * 0.08, tip = left + (bot - top) * 0.5, right = w - m * 0.5;
        QPainterPath path;
        path.moveTo(left, h / 2);
        path.lineTo(tip, top);
        path.lineTo(right, top);
        path.lineTo(right, bot);
        path.lineTo(tip, bot);
        path.closeSubpath();
        p.fillPath(path, c);
        QPen pen(palette().color(QPalette::Base), qMax<qreal>(1.2, h * 0.09), Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        const qreal cx = (tip + right) / 2 - h * 0.02, cy = h / 2, r = (bot - top) * 0.22;
        p.drawLine(QPointF(cx - r, cy - r), QPointF(cx + r, cy + r));
        p.drawLine(QPointF(cx - r, cy + r), QPointF(cx + r, cy - r));
    }

private:
    void relayout()
    {
        const int h = QFontMetrics(le_->font()).height();
        const int bw = int(h * 1.25), bh = h;
        resize(bw, bh);
        move(le_->width() - bw - 4, (le_->height() - bh) / 2);
        le_->setTextMargins(0, 0, bw + 2, 0);
    }

    QLineEdit *le_;
};

class MainWindow : public QWidget {
    friend struct EditorTest;

public:
    MainWindow()
    {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        editor_ = new Editor(this);
        lay->addWidget(editor_);
        buildFindBar();
        retranslate();
        lay->addWidget(bar_);
        bar_->hide();

        connect(editor_, &Editor::zoomChanged, this, &MainWindow::scaleBar);
        connect(editor_, &Editor::contextMenuAboutToShow, this, &MainWindow::extendContextMenu);
        connect(editor_, &Editor::modifiedChanged, this, [this](bool) { updateTitle(); });
        connect(editor_, &Editor::fileDropped, this, [this](const QString &p) {
            if (!editor_->isBusy() && maybeSave())
                openPath(p);
        });

        auto sc = [this](const QKeySequence &k, auto fn) {
            auto *s = new QShortcut(k, this);
            s->setContext(Qt::WindowShortcut);
            connect(s, &QShortcut::activated, this, [this, fn] {
                if (!editor_->isBusy())  // the document must not change while a worker reads it
                    fn();
            });
        };
        // Esc: cancel a long operation, else close the find bar, else drop the selection.
        auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(esc, &QShortcut::activated, this, [this] {
            if (editor_->isBusy())
                editor_->cancelBusy();
            else if (bar_->isVisible())
                hideBar();
            else
                editor_->clearSelection();
        });
        connect(editor_, &Editor::busyChanged, this, [this](const QString &what, int percent) {
            if (percent < 0 && !busyText_.isEmpty() && status_->text() == busyText_)
                status_->clear();
            busyText_ = percent < 0 ? QString() : tr("%1 %2 % (Esc cancels)").arg(what).arg(percent);
            if (percent >= 0)
                status_->setText(busyText_);
            updateTitle();
            if (percent < 0 && closePending_)
                QTimer::singleShot(0, this, &QWidget::close);
        });
        sc(QKeySequence::Save, [this] { save(false); });
        sc(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] { save(true); });
        sc(QKeySequence::Open, [this] {
            QString p = chooseFile(this, false, path_);
            if (!p.isEmpty() && maybeSave())
                openPath(p);
        });
        sc(QKeySequence::New, [] { QProcess::startDetached(QCoreApplication::applicationFilePath(), {}); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_W), [this] { close(); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_Q), [this] { close(); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_F), [this] { showBar(false); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_H), [this] { showBar(true); });
        sc(QKeySequence(Qt::Key_F3), [this] { findNext(false); });
        sc(QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { findNext(true); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_0), [this] { editor_->setZoom(100); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_Plus), [this] { editor_->setZoom(editor_->zoom() + 10); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this] { editor_->setZoom(editor_->zoom() + 10); });
        sc(QKeySequence(Qt::CTRL | Qt::Key_Minus), [this] { editor_->setZoom(editor_->zoom() - 10); });
        sc(QKeySequence(Qt::ALT | Qt::Key_Z), [this] { editor_->setWordWrap(!editor_->wordWrap()); });
        sc(QKeySequence(Qt::ALT | Qt::Key_C), [this] { setInverted(!editor_->inverted()); });

        QSettings st;
        editor_->setZoom(st.value("zoom", 100).toInt());
        scaleBar(editor_->zoom());
        if (!st.value("wrap", true).toBool())
            editor_->setWordWrap(false);
        if (st.value("inverted", false).toBool())
            setInverted(true);
        resize(st.value("size", QSize(900, 650)).toSize());
        setFocusProxy(editor_);
        updateTitle();
    }

    void openPath(const QString &p)
    {
        QFileInfo fi(p);
        QString abs = fi.exists() ? fi.canonicalFilePath() : fi.absoluteFilePath();
        if (!fi.exists()) {  // new file with that name
            path_ = abs;
            updateTitle();
            return;
        }
        QString err;
        if (!editor_->openFile(abs, err)) {
            QMessageBox::warning(this, QStringLiteral("Zen Notepad"), tr("Cannot open “%1”:\n%2").arg(p, err));
            return;
        }
        path_ = abs;
        updateTitle();
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (editor_->isBusy()) {  // finish (cancel) the running operation first
            closePending_ = true;
            editor_->cancelBusy();
            e->ignore();
            return;
        }
        closePending_ = false;
        if (!maybeSave()) {
            e->ignore();
            return;
        }
        QSettings st;
        st.setValue("zoom", editor_->zoom());
        st.setValue("size", size());
        st.setValue("wrap", editor_->wordWrap());
        st.setValue("inverted", editor_->inverted());
        e->accept();
    }

private:
    void setInverted(bool on)
    {
        editor_->setInverted(on);
        QPalette p = QApplication::palette();
        if (on) {
            p.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));
            p.setColor(QPalette::WindowText, Qt::white);
            p.setColor(QPalette::Base, QColor(0x10, 0x10, 0x10));
            p.setColor(QPalette::AlternateBase, QColor(0x28, 0x28, 0x28));
            p.setColor(QPalette::Text, Qt::white);
            p.setColor(QPalette::Button, QColor(0x30, 0x30, 0x30));
            p.setColor(QPalette::ButtonText, Qt::white);
            p.setColor(QPalette::PlaceholderText, QColor(0x90, 0x90, 0x90));
            p.setColor(QPalette::ToolTipBase, QColor(0x30, 0x30, 0x30));
            p.setColor(QPalette::ToolTipText, Qt::white);
        }
        setPalette(p);
        setStatus(status_->text(), badStatus_);
    }

    void updateTitle()
    {
        QString name = path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName();
        setWindowTitle((busyText_.isEmpty() ? QString() : QStringLiteral("[") + busyText_ + QStringLiteral("] ")) +
                       (editor_->isModified() ? QStringLiteral("*") : QString()) + name + QStringLiteral(" – Zen Notepad"));
    }

    bool save(bool as)
    {
        QString p = path_;
        if (as || p.isEmpty()) {
            p = chooseFile(this, true, path_);
            if (p.isEmpty())
                return false;
        }
        QString err;
        if (!editor_->saveFile(p, err)) {
            if (editor_->wasCancelled())
                return false;
            QMessageBox::warning(this, QStringLiteral("Zen Notepad"), tr("Cannot save “%1”:\n%2").arg(p, err));
            return false;
        }
        path_ = p;
        updateTitle();
        return true;
    }

    bool maybeSave()
    {
        if (!editor_->isModified())
            return true;
        QString name = path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName();
        QMessageBox mb(QMessageBox::Question, QStringLiteral("Zen Notepad"), tr("Save changes to “%1”?").arg(name),
                       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
        mb.button(QMessageBox::Save)->setText(tr("Save"));
        mb.button(QMessageBox::Discard)->setText(tr("Don't Save"));
        mb.button(QMessageBox::Cancel)->setText(tr("Cancel"));
        mb.setDefaultButton(QMessageBox::Save);
        switch (mb.exec()) {
        case QMessageBox::Save: return save(false);
        case QMessageBox::Discard: return true;
        default: return false;
        }
    }

    // ------------------------------------------------------------ find bar

    QToolButton *toolButton(const QString &text, const QString &tip, bool checkable = false)
    {
        auto *b = new QToolButton(bar_);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::TabFocus);
        return b;
    }

    void buildFindBar()
    {
        bar_ = new QWidget(this);
        auto *v = new QVBoxLayout(bar_);
        v->setContentsMargins(6, 4, 6, 4);
        v->setSpacing(4);
        auto *r1 = new QHBoxLayout;
        auto *r2 = new QHBoxLayout;
        v->addLayout(r1);
        v->addLayout(r2);

        find_ = new QLineEdit(bar_);
        new LineClearButton(find_);
        case_ = toolButton(QStringLiteral("Aa"), QString(), true);
        regex_ = toolButton(QStringLiteral(".*"), QString(), true);
        auto *first = first_ = toolButton(QStringLiteral("↧"), QString());
        auto *next = next_ = toolButton(QStringLiteral("↓"), QString());
        auto *prev = prev_ = toolButton(QStringLiteral("↑"), QString());
        auto *last = last_ = toolButton(QStringLiteral("↥"), QString());
        status_ = new QLabel(bar_);
        auto *close = close_ = toolButton(QStringLiteral("✕"), QString());
        r1->addWidget(find_, 1);
        r1->addWidget(case_);
        r1->addWidget(regex_);
        r1->addWidget(first);
        r1->addWidget(next);
        r1->addWidget(prev);
        r1->addWidget(last);
        r1->addWidget(status_);
        r1->addWidget(close);

        repl_ = new QLineEdit(bar_);
        auto *one = one_ = toolButton(QString(), QString());
        auto *all = all_ = toolButton(QString(), QString());
        r2->setContentsMargins(0, 0, 0, 0);
        new LineClearButton(repl_);
        r2->addWidget(repl_, 1);
        r2->addWidget(one);
        r2->addWidget(all);
        replWidgets_ = {repl_, one, all};

        connect(find_, &QLineEdit::returnPressed, this, [this] {
            findFirst(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier);
        });
        connect(first, &QToolButton::clicked, this, [this] { findFirst(false); });
        connect(last, &QToolButton::clicked, this, [this] { findFirst(true); });
        connect(find_, &QLineEdit::textChanged, this, [this] { setStatus(QString(), false); });
        connect(prev, &QToolButton::clicked, this, [this] { findNext(true); });
        connect(next, &QToolButton::clicked, this, [this] { findNext(false); });
        connect(close, &QToolButton::clicked, this, &MainWindow::hideBar);
        connect(repl_, &QLineEdit::returnPressed, this, [this] {
            auto m = QGuiApplication::keyboardModifiers();
            if ((m & Qt::ControlModifier) && (m & Qt::AltModifier))
                replaceAll();
            else
                replaceFirst(m & Qt::ShiftModifier);
        });
        connect(one, &QToolButton::clicked, this, &MainWindow::replaceOne);
        connect(all, &QToolButton::clicked, this, &MainWindow::replaceAll);
        connect(case_, &QToolButton::toggled, this, [this] { setStatus(QString(), false); });
        connect(regex_, &QToolButton::toggled, this, [this] { setStatus(QString(), false); });

        auto *all2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Return), this);
        connect(all2, &QShortcut::activated, this, [this] {
            if (!editor_->isBusy() && bar_->isVisible() && repl_->isVisible())
                replaceAll();
        });
    }

    // The find bar grows and shrinks together with the text (Ctrl+wheel).
    void scaleBar(int percent)
    {
        QFont f = QApplication::font();
        f.setPointSizeF(f.pointSizeF() * percent / 100.0);
        bar_->setFont(f);
        int h = QFontMetrics(f).height();
        for (QToolButton *b : bar_->findChildren<QToolButton *>())
            b->setMinimumHeight(h + 8);
    }

    void retranslate()
    {
        find_->setPlaceholderText(tr("Find"));
        case_->setToolTip(tr("Match case"));
        regex_->setToolTip(tr("Regular expression (PCRE2; in replacement $1, ${name}, \\n, \\t)"));
        first_->setToolTip(tr("First match from the beginning (Enter)"));
        next_->setToolTip(tr("Next match (F3)"));
        prev_->setToolTip(tr("Previous match (Shift+F3)"));
        last_->setToolTip(tr("Last match from the end (Shift+Enter)"));
        repl_->setToolTip(tr("Enter: replace the first match from the beginning\n"
                             "Shift+Enter: replace the last match from the end\n"
                             "Ctrl+Alt+Enter: replace all"));
        close_->setToolTip(tr("Close (Esc)"));
        repl_->setPlaceholderText(tr("Replace with"));
        one_->setText(tr("Replace"));
        one_->setToolTip(tr("Replace the selected match and go to the next one"));
        all_->setText(tr("Replace All"));
        all_->setToolTip(tr("Replace all (Ctrl+Alt+Enter)"));
        setStatus(QString(), false);
        updateTitle();
    }

    void extendContextMenu(QMenu *m)
    {
        m->addSeparator();
        QAction *a = m->addAction(tr("Word Wrap"), this, [this] { editor_->setWordWrap(!editor_->wordWrap()); });
        a->setCheckable(true);
        a->setChecked(editor_->wordWrap());
        a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Z));
        a = m->addAction(tr("Inverted Colors"), this, [this] { setInverted(!editor_->inverted()); });
        a->setCheckable(true);
        a->setChecked(editor_->inverted());
        a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_C));

        QMenu *dm = m->addMenu(tr("Text Direction"));
        const QString dirs[] = {tr("Automatic"), tr("Left to Right"), tr("Right to Left")};
        for (int i = 0; i < 3; ++i) {
            QAction *d = dm->addAction(dirs[i], this, [this, i] { editor_->setDirectionMode(i); });
            d->setCheckable(true);
            d->setChecked(editor_->directionMode() == i);
        }

        QMenu *lm = m->addMenu(tr("Language"));
        const QString cur = uiLanguage();
        std::vector<std::pair<QString, QString>> langs{{QStringLiteral("auto"), tr("Automatic (system)")}};
        for (const auto &l : availableLanguages())
            langs.push_back(l);
        for (const auto &[code, name] : langs) {
            QAction *a = lm->addAction(name, this, [this, code] {
                setUiLanguage(code);
                QSettings().setValue("language", code);
                retranslate();
            });
            a->setCheckable(true);
            a->setChecked(code == cur);
        }
    }

    void showBar(bool replace)
    {
        QString sel = editor_->selectedText(1000);
        if (!sel.isEmpty() && !sel.contains(QLatin1Char('\n')))
            find_->setText(sel);
        for (QWidget *w : replWidgets_)
            w->setVisible(replace);
        bar_->show();
        if (replace && !find_->text().isEmpty() && !sel.isEmpty())
            repl_->setFocus();
        else
            find_->setFocus();
        find_->selectAll();
    }

    void hideBar()
    {
        bar_->hide();
        editor_->setFocus();
    }

    void setStatus(const QString &s, bool bad)
    {
        status_->setText(s);
        badStatus_ = bad;
        QPalette p = palette();
        if (bad)
            p.setColor(QPalette::Base, editor_->inverted() ? QColor(0x60, 0x18, 0x18) : QColor(255, 220, 220));
        find_->setPalette(p);
    }

    bool prepare(Searcher &s)
    {
        if (editor_->isBusy())
            return false;
        if (find_->text().isEmpty()) {
            if (!bar_->isVisible())
                showBar(false);
            return false;
        }
        std::string err;
        if (!s.setPattern(find_->text().toUtf8().toStdString(), case_->isChecked(), regex_->isChecked(), err)) {
            setStatus(QString::fromUtf8(err.c_str()), true);
            return false;
        }
        return true;
    }

    void findNext(bool backward)
    {
        Searcher s(editor_->document());
        if (!prepare(s))
            return;
        bool wrapped;
        if (editor_->find(s, backward, wrapped))
            setStatus(wrapped ? (backward ? tr("Wrapped around to the end") : tr("Wrapped around to the beginning")) : QString(), false);
        else
            setStatus(editor_->wasCancelled() ? tr("Cancelled") : tr("Not found"), !editor_->wasCancelled());
    }

    void findFirst(bool fromEnd)
    {
        Searcher s(editor_->document());
        if (!prepare(s))
            return;
        if (editor_->findFirst(s, fromEnd))
            setStatus(QString(), false);
        else
            setStatus(editor_->wasCancelled() ? tr("Cancelled") : tr("Not found"), !editor_->wasCancelled());
    }

    void replaceFirst(bool fromEnd)
    {
        Searcher s(editor_->document());
        if (!prepare(s))
            return;
        if (editor_->replaceFirst(s, repl_->text().toUtf8().toStdString(), fromEnd))
            setStatus(QString(), false);
        else
            setStatus(editor_->wasCancelled() ? tr("Cancelled") : tr("Not found"), !editor_->wasCancelled());
    }

    void replaceOne()
    {
        Searcher s(editor_->document());
        if (!prepare(s))
            return;
        bool wrapped;
        if (editor_->replaceOne(s, repl_->text().toUtf8().toStdString(), wrapped))
            setStatus(wrapped ? tr("Wrapped around to the beginning") : QString(), false);
        else
            setStatus(editor_->wasCancelled() ? tr("Cancelled") : tr("Not found"), !editor_->wasCancelled());
    }

    void replaceAll()
    {
        Searcher s(editor_->document());
        if (!prepare(s))
            return;
        uint64_t n = editor_->replaceAll(s, repl_->text().toUtf8().toStdString());
        if (n)
            setStatus(tr("Replaced: %1").arg(n), false);
        else
            setStatus(editor_->wasCancelled() ? tr("Cancelled") : tr("Not found"), !editor_->wasCancelled());
    }

    Editor *editor_;
    QWidget *bar_;
    QLineEdit *find_, *repl_;
    QToolButton *case_, *regex_;
    QLabel *status_;
    QString busyText_;
    bool closePending_ = false;
    QToolButton *first_, *next_, *prev_, *last_, *close_, *one_, *all_;
    bool badStatus_ = false;
    QList<QWidget *> replWidgets_;
    QString path_;
};

