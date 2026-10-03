#include "editor.h"
#include "search.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTextLine>
#include <cmath>

namespace {
const uint64_t kL = 32768;       // max block length before a forced break
const uint64_t kAdjust = 512;    // forced breaks prefer a space within this distance
const uint64_t kNone = ~uint64_t(0);

void decodeUtf8(const char *s, size_t n, QString &out, std::vector<uint32_t> &map)
{
    out.resize(n);
    map.resize(n + 1);
    QChar *o = out.data();
    size_t k = 0, i = 0;
    const unsigned char *u = reinterpret_cast<const unsigned char *>(s);
    while (i < n) {
        unsigned c = u[i];
        if (c < 0x80) {
            map[k] = i;
            o[k++] = QChar(c);
            ++i;
            continue;
        }
        unsigned cp = 0, len = 0, min = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; min = 0x10000; }
        bool ok = len && i + len <= n;
        for (unsigned j = 1; ok && j < len; ++j) {
            if ((u[i + j] & 0xC0) != 0x80)
                ok = false;
            else
                cp = (cp << 6) | (u[i + j] & 0x3F);
        }
        if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)))
            ok = false;
        if (!ok) {
            map[k] = i;
            o[k++] = QChar(0xFFFD);
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            map[k] = i;
            o[k++] = QChar(QChar::highSurrogate(cp));
            map[k] = i;
            o[k++] = QChar(QChar::lowSurrogate(cp));
        } else {
            map[k] = i;
            o[k++] = QChar(cp);
        }
        i += len;
    }
    out.resize(k);
    map.resize(k + 1);
    map[k] = n;
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c.isMark() || c == QLatin1Char('_') || c.isSurrogate();
}
int charClass(QChar c)
{
    if (c.isSpace())
        return 0;
    return isWordChar(c) ? 1 : 2;
}
} // namespace

Editor::Editor(QWidget *parent) : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);

    font_ = QApplication::font();
    basePointSize_ = qMax<qreal>(font_.pointSizeF(), 11.0);
    font_.setPointSizeF(basePointSize_);
    opt_.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setDirection(false);

    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (updatingSb_)
            return;
        xOff_ = v;
        viewport()->update();
    });
    QScrollBar *sb = verticalScrollBar();
    connect(sb, &QScrollBar::valueChanged, this, [this](int v) {
        if (updatingSb_ || v == sbLastSet_)
            return;
        scrollToValue(v);
    });
    connect(sb, &QScrollBar::actionTriggered, this, [this, sb](int action) {
        switch (action) {
        case QAbstractSlider::SliderSingleStepAdd: scrollRows(1); break;
        case QAbstractSlider::SliderSingleStepSub: scrollRows(-1); break;
        case QAbstractSlider::SliderPageStepAdd: scrollRows(rowsPerPage()); break;
        case QAbstractSlider::SliderPageStepSub: scrollRows(-rowsPerPage()); break;
        case QAbstractSlider::SliderToMinimum: top_ = Anchor{}; break;
        case QAbstractSlider::SliderToMaximum: top_ = maxTop(); break;
        default: return;
        }
        int v = sbValueOf(top_);
        sbLastSet_ = v;
        sb->setSliderPosition(v);
        viewport()->update();
    });
    resetState();
}

Editor::~Editor() = default;

// ---------------------------------------------------------------- file

void Editor::resetState()
{
    undo_.clear();
    redo_.clear();
    cleanIndex_ = 0;
    cursor_ = anchor_ = 0;
    top_ = Anchor{};
    xOff_ = hRange_ = 0;
    prefX_ = -1;
    preedit_.clear();
    breakMerge_ = true;
    const uint64_t size = doc_.size();
    int64_t nl = doc_.findForward('\n', 0, 1 << 20);
    eol_ = (nl > 0 && doc_.byteAt(nl - 1) == '\r') ? "\r\n" : "\n";
    strongL_ = strongR_ = 0;
    trackDir_ = size <= (8u << 20);
    countDirRange(0, trackDir_ ? size : std::min<uint64_t>(size, 1 << 20), 1);
    bool rtl = dirMode_ == 0 ? strongR_ > strongL_ : dirMode_ == 2;
    rtl_ = !rtl;  // force setDirection to apply
    setDirection(rtl);
    emit modifiedChanged(false);
}

bool Editor::openFile(const QString &path, QString &err)
{
    std::string e;
    if (!doc_.open(QFile::encodeName(path).toStdString(), e)) {
        err = QString::fromLocal8Bit(e.c_str());
        return false;
    }
    resetState();
    return true;
}

bool Editor::saveFile(const QString &path, QString &err)
{
    std::string e;
    bool inval = false;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = doc_.save(QFile::encodeName(path).toStdString(), e, inval);
    QGuiApplication::restoreOverrideCursor();
    if (inval) {
        undo_.clear();
        redo_.clear();
        invalidateLayout();
    }
    if (!ok) {
        err = QString::fromLocal8Bit(e.c_str());
        return false;
    }
    cleanIndex_ = undo_.size();
    emit modifiedChanged(false);
    return true;
}

QString Editor::selectedText(uint64_t limit) const
{
    if (!hasSelection() || selEnd() - selStart() > limit)
        return QString();
    return QString::fromUtf8(doc_.text(selStart(), selEnd()));
}

// ---------------------------------------------------------------- direction

void Editor::countDir(const char *p, uint64_t n, int sign)
{
    const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
    int64_t l = 0, r = 0;
    for (uint64_t i = 0; i < n;) {
        unsigned c = u[i];
        if (c < 0x80) {
            if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
                ++l;
            ++i;
            continue;
        }
        unsigned cp, len;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > n) break;
        for (unsigned j = 1; j < len; ++j)
            cp = (cp << 6) | (u[i + j] & 0x3F);
        i += len;
        switch (QChar::direction(char32_t(cp))) {
        case QChar::DirL: ++l; break;
        case QChar::DirR:
        case QChar::DirAL: ++r; break;
        default: break;
        }
    }
    strongL_ += sign * l;
    strongR_ += sign * r;
}

void Editor::countDir(const std::vector<Piece> &ps, int sign)
{
    for (const Piece &p : ps)
        countDir(doc_.bufData(p.buf) + p.off, p.len, sign);
}

void Editor::countDirRange(uint64_t a, uint64_t b, int sign)
{
    while (a < b) {
        uint64_t av;
        const char *p = doc_.chunkAt(a, av);
        if (!p)
            break;
        uint64_t n = std::min(av, b - a);
        countDir(p, n, sign);
        a += n;
    }
}

void Editor::setDirection(bool rtl)
{
    if (rtl == rtl_)
        return;
    rtl_ = rtl;
    opt_.setTextDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
    opt_.setAlignment((rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignAbsolute);
    setZoom(zoom_);  // refreshes tab stops and layouts
}

void Editor::setZoom(int percent)
{
    percent = qBound(30, percent, 500);
    bool changed = percent != zoom_;
    zoom_ = percent;
    hRange_ = 0;
    font_.setPointSizeF(basePointSize_ * zoom_ / 100.0);
    opt_.setTabStopDistance(QFontMetricsF(font_).horizontalAdvance(QLatin1Char(' ')) * 4);
    invalidateLayout();
    if (changed)
        emit zoomChanged(zoom_);
}

void Editor::setWordWrap(bool on)
{
    wrap_ = on;
    xOff_ = hRange_ = 0;
    opt_.setWrapMode(on ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    setHorizontalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    invalidateLayout();
    ensureCursorVisible();
}

void Editor::setDirectionMode(int mode)
{
    dirMode_ = mode;
    setDirection(mode == 0 ? strongR_ > strongL_ : mode == 2);
    ensureCursorVisible();
}

void Editor::setInverted(bool on)
{
    inverted_ = on;
    viewport()->update();
}

// ---------------------------------------------------------------- blocks

bool Editor::forcedBoundary(uint64_t k, uint64_t &b) const
{
    const uint64_t size = doc_.size();
    uint64_t x = k * kL;
    if (k == 0 || x >= size)
        return false;
    if (doc_.findForward('\n', x - kL, x + 1) >= 0)
        return false;
    int64_t q = doc_.findBackward(' ', x, x - kAdjust);
    if (q >= 0) {
        b = q + 1;
    } else {
        b = x;
        while (b > x - 3 && (doc_.byteAt(b) & 0xC0) == 0x80)
            --b;
    }
    return true;
}

uint64_t Editor::blockStartOf(uint64_t p) const
{
    const uint64_t size = doc_.size();
    if (p > size)
        p = size;
    uint64_t lo = p > 2 * kL ? p - 2 * kL : 0;
    int64_t nl = doc_.findBackward('\n', p, lo);
    uint64_t kTop = (p + kAdjust) / kL + 1;
    if (nl < 0 && lo > 0) {
        for (uint64_t k = kTop; k >= 1 && k * kL >= lo; --k) {
            uint64_t b;
            if (forcedBoundary(k, b) && b <= p)
                return b;
        }
        nl = doc_.findBackward('\n', lo, 0);
    }
    uint64_t ls = nl >= 0 ? uint64_t(nl) + 1 : 0;
    for (uint64_t k = kTop; k >= 1 && k * kL >= ls; --k) {
        if (k * kL < ls + kL)  // newline at ls-1 lies inside the validity window
            break;
        uint64_t b;
        if (forcedBoundary(k, b) && b <= p)
            return b;
    }
    return ls;
}

void Editor::blockBounds(uint64_t s, uint64_t &end, uint64_t &next, bool &last) const
{
    const uint64_t size = doc_.size();
    int64_t nl = doc_.findForward('\n', s, s + 2 * kL + 1);
    uint64_t limit = nl >= 0 ? uint64_t(nl) : std::min(size, s + 2 * kL + 1);
    bool lineStart = s == 0 || doc_.byteAt(s - 1) == '\n';
    for (uint64_t k = s / kL + 1; k * kL < size && k * kL <= limit + kAdjust; ++k) {
        if (lineStart && k * kL < s + kL)
            continue;
        if (nl >= 0 && k * kL >= uint64_t(nl))
            break;
        uint64_t b;
        if (forcedBoundary(k, b) && b > s) {
            end = next = b;
            last = false;
            return;
        }
    }
    if (nl >= 0) {
        end = nl;
        if (end > s && doc_.byteAt(end - 1) == '\r')
            --end;
        next = nl + 1;
        last = false;
    } else {
        end = next = size;
        last = true;
    }
}

BlockP Editor::block(uint64_t s)
{
    auto it = cache_.find(s);
    if (it != cache_.end())
        return it->second;
    if (cache_.size() > 3000)
        cache_.clear();
    auto b = std::make_shared<Block>();
    b->start = s;
    blockBounds(s, b->end, b->next, b->last);
    std::string sc;
    std::string_view sv = doc_.view(s, b->end, sc);
    decodeUtf8(sv.data(), b->end > s ? sv.size() : 0, b->text, b->u16);
    QTextLayout &l = b->layout;
    l.setText(b->text);
    l.setFont(font_);
    l.setTextOption(opt_);
    l.setCacheEnabled(true);
    if (!preedit_.isEmpty() && cursor_ >= s && cursor_ <= b->end && blockStartOf(cursor_) == s) {
        int idx = idxOf(*b, cursor_);
        l.setPreeditArea(idx, preedit_);
        QList<QTextLayout::FormatRange> fr;
        for (const auto &a : preeditAttrs_) {
            if (a.type != QInputMethodEvent::TextFormat)
                continue;
            QTextCharFormat f = qvariant_cast<QTextFormat>(a.value).toCharFormat();
            if (f.isValid())
                fr.append({idx + a.start, a.length, f});
        }
        if (fr.isEmpty()) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            fr.append({idx, int(preedit_.size()), f});
        }
        l.setFormats(fr);
    }
    // Rows are at least one line of the main font high, so that fallback fonts
    // (CJK, Devanagari, ...) do not make line spacing uneven.
    qreal w = qMax(20, viewport()->width() - 2 * kMargin);
    qreal minH = QFontMetricsF(font_).lineSpacing();
    qreal h = 0;
    l.beginLayout();
    for (;;) {
        QTextLine line = l.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(w);
        qreal rh = qMax(minH, std::ceil(line.height()));
        line.setPosition(QPointF(0, h + (rh - line.height()) / 2));
        b->rows.push_back(h);
        h += rh;
    }
    l.endLayout();
    b->rows.push_back(h);
    b->height = h;
    cache_[s] = b;
    return b;
}

int Editor::idxOf(const Block &b, uint64_t pos) const
{
    if (pos <= b.start)
        return 0;
    uint64_t rel = std::min(pos, b.end) - b.start;
    auto it = std::lower_bound(b.u16.begin(), b.u16.end(), uint32_t(rel));
    return int(it - b.u16.begin());
}

void Editor::invalidateLayout()
{
    cache_.clear();
    maxTopValid_ = false;
    clampTop();
    updateScrollBar();
    viewport()->update();
}

// ---------------------------------------------------------------- rows & scrolling

bool Editor::nextRow(Anchor &a)
{
    BlockP b = block(a.block);
    if (a.line + 1 < b->lineCount()) {
        ++a.line;
        return true;
    }
    if (b->last)
        return false;
    a = Anchor{b->next, 0};
    return true;
}

bool Editor::prevRow(Anchor &a)
{
    if (a.line > 0) {
        --a.line;
        return true;
    }
    if (a.block == 0)
        return false;
    uint64_t s = blockStartOf(a.block - 1);
    a = Anchor{s, block(s)->lineCount() - 1};
    return true;
}

qreal Editor::rowHeight(const Anchor &a)
{
    BlockP b = block(a.block);
    int i = qBound(0, a.line, b->lineCount() - 1);
    return b->rows[i + 1] - b->rows[i];
}

Editor::Anchor Editor::anchorOf(uint64_t pos)
{
    BlockP b = blockAt(pos);
    int idx = idxOf(*b, pos);
    if (!preedit_.isEmpty() && pos == cursor_)
        idx += preeditCursor_;
    QTextLine l = b->layout.lineForTextPosition(idx);
    return Anchor{b->start, l.isValid() ? l.lineNumber() : b->lineCount() - 1};
}

Editor::Anchor Editor::maxTop()
{
    if (maxTopValid_)
        return maxTop_;
    BlockP last = blockAt(doc_.size());
    Anchor a{last->start, last->lineCount() - 1};
    qreal vh = viewport()->height();
    qreal h = rowHeight(a);
    Anchor p = a;
    while (prevRow(p)) {
        qreal rh = rowHeight(p);
        if (h + rh > vh)
            break;
        h += rh;
        a = p;
    }
    maxTop_ = a;
    maxTopValid_ = true;
    return a;
}

void Editor::clampTop()
{
    top_.block = blockStartOf(top_.block);
    BlockP b = block(top_.block);
    top_.line = qBound(0, top_.line, b->lineCount() - 1);
    Anchor m = maxTop();
    if (m < top_)
        top_ = m;
}

void Editor::ensureCursorVisible()
{
    Anchor c = anchorOf(cursor_);
    qreal vh = viewport()->height();
    if (c < top_) {
        top_ = c;
    } else {
        Anchor a = top_;
        qreal y = 0;
        bool vis = false;
        while (y < vh) {
            if (a == c) {
                vis = y + rowHeight(c) <= vh || a == top_;
                break;
            }
            y += rowHeight(a);
            if (!nextRow(a))
                break;
        }
        if (!vis) {
            Anchor t = c, p = c;
            qreal h = rowHeight(c);
            while (prevRow(p)) {
                qreal rh = rowHeight(p);
                if (h + rh > vh)
                    break;
                h += rh;
                t = p;
            }
            top_ = t;
        }
    }
    clampTop();
    if (!wrap_) {
        BlockP b = blockAt(cursor_);
        int idx = idxOf(*b, cursor_) + (preedit_.isEmpty() ? 0 : preeditCursor_);
        QTextLine l = b->layout.lineForTextPosition(idx);
        if (l.isValid()) {
            qreal x = kMargin + xShift() + l.cursorToX(idx);
            qreal vw = viewport()->width(), pad = qMin<qreal>(vw / 4, 4 * QFontMetricsF(font_).averageCharWidth());
            qreal d = 0;
            if (x < kMargin)
                d = x - kMargin - pad;
            else if (x > vw - kMargin)
                d = x - (vw - kMargin) + pad;
            if (d != 0) {
                xOff_ += int(rtl_ ? -d : d);
                if (xOff_ < 0)
                    xOff_ = 0;
            }
            if (l.lineNumber() == 0 && idx == 0 && !rtl_)
                xOff_ = 0;
        }
    }
    updateScrollBar();
    viewport()->update();
}

void Editor::scrollRows(int n)
{
    for (; n > 0 && nextRow(top_); --n) {}
    for (; n < 0 && prevRow(top_); ++n) {}
    clampTop();
    updateScrollBar();
    viewport()->update();
}

int Editor::rowsPerPage()
{
    Anchor a = top_;
    qreal y = 0, vh = viewport()->height();
    int n = 0;
    while (y + rowHeight(a) <= vh) {
        y += rowHeight(a);
        ++n;
        if (!nextRow(a))
            break;
    }
    return qMax(1, n - 1);
}

int Editor::sbValueOf(const Anchor &a)
{
    BlockP b = block(a.block);
    uint64_t len = (b->last ? doc_.size() : b->next) - b->start;
    uint64_t v = a.block + uint64_t(double(len) * a.line / qMax(1, b->lineCount()));
    return int(v / sbUnit_);
}

void Editor::updateScrollBar()
{
    updatingSb_ = true;
    QScrollBar *sb = verticalScrollBar();
    sbUnit_ = doc_.size() / (1u << 30) + 1;
    int mx = sbValueOf(maxTop());
    if (mx == 0 && !(maxTop() == Anchor{}))
        mx = 1;
    sb->setRange(0, mx);
    sb->setPageStep(int(qMax<uint64_t>(1, pageBytes_ / sbUnit_)));
    sb->setSingleStep(1);
    int v = top_ == maxTop() ? mx : sbValueOf(top_);
    sbLastSet_ = v;
    sb->setValue(v);
    updatingSb_ = false;
    updateHScroll();
}

void Editor::updateHScroll()
{
    QScrollBar *hb = horizontalScrollBar();
    if (wrap_) {
        xOff_ = 0;
        return;
    }
    // Range from the widest line seen so far (the whole file may be huge). It never shrinks while
    // scrolling: a scrollbar that appears and disappears would resize the viewport each time and make
    // the vertical scrollbar jump. It is reset when the file, zoom or wrap mode changes.
    qreal textW = viewport()->width() - 2 * kMargin, over = 0, y = 0, vh = viewport()->height();
    Anchor a = top_;
    while (y < vh) {
        BlockP b = block(a.block);
        QTextLine l = b->layout.lineAt(a.line);
        over = qMax(over, l.naturalTextWidth() + kMargin - textW);
        y += rowHeight(a);
        if (!nextRow(a))
            break;
    }
    updatingSb_ = true;
    hb->setInvertedAppearance(rtl_);
    hRange_ = qMax(hRange_, qMax(int(std::ceil(over)), xOff_));
    hb->setRange(0, hRange_);
    hb->setPageStep(int(textW));
    hb->setSingleStep(qMax(8, int(QFontMetricsF(font_).averageCharWidth() * 3)));
    hb->setValue(xOff_);
    updatingSb_ = false;
}

void Editor::scrollToValue(int v)
{
    QScrollBar *sb = verticalScrollBar();
    if (v >= sb->maximum()) {
        top_ = maxTop();
    } else {
        uint64_t p = uint64_t(v) * sbUnit_;
        uint64_t s = blockStartOf(p);
        BlockP b = block(s);
        uint64_t len = qMax<uint64_t>(1, (b->last ? doc_.size() : b->next) - s);
        top_ = Anchor{s, int((p - s) * b->lineCount() / len)};
    }
    clampTop();
    updateHScroll();
    viewport()->update();
}

// ---------------------------------------------------------------- painting

void Editor::paintEvent(QPaintEvent *e)
{
    QPainter p(viewport());
    p.fillRect(e->rect(), inverted_ ? Qt::black : Qt::white);
    p.setPen(inverted_ ? Qt::white : Qt::black);
    const qreal xs = kMargin + xShift();
    const qreal vh = viewport()->height();
    const QPalette pal = palette();
    const uint64_t s0 = selStart(), s1 = selEnd();

    BlockP b = block(top_.block);
    qreal y = -b->rows[qBound(0, top_.line, b->lineCount() - 1)];
    uint64_t bottom = b->start;
    for (;;) {
        if (y + b->height >= e->rect().top()) {
            QList<QTextLayout::FormatRange> sel;
            if (s0 != s1 && s0 <= b->end && s1 > b->start) {
                int i0 = idxOf(*b, s0), i1 = idxOf(*b, s1);
                QTextLayout::FormatRange r;
                r.start = i0;
                r.length = i1 - i0;
                r.format.setBackground(pal.brush(QPalette::Active, QPalette::Highlight));
                r.format.setForeground(pal.brush(QPalette::Active, QPalette::HighlightedText));
                if (r.length > 0)
                    sel.append(r);
            }
            b->layout.draw(&p, QPointF(xs, y), sel, e->rect());
            if (hasFocus() && cursorOn_ && cursor_ >= b->start && cursor_ <= b->end &&
                blockStartOf(cursor_) == b->start) {
                int idx = idxOf(*b, cursor_) + (preedit_.isEmpty() ? 0 : preeditCursor_);
                b->layout.drawCursor(&p, QPointF(xs, y), idx, 1);
            }
        }
        y += b->height;
        bottom = b->last ? doc_.size() : b->next;
        if (y >= vh || b->last)
            break;
        b = block(b->next);
    }
    pageBytes_ = qMax<uint64_t>(1, bottom - top_.block);
}

void Editor::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    invalidateLayout();
}

QRect Editor::cursorRect() const
{
    Editor *self = const_cast<Editor *>(this);
    Anchor c = self->anchorOf(cursor_);
    if (c < top_)
        return QRect();
    Anchor a = top_;
    qreal y = 0, vh = viewport()->height();
    while (!(a == c) && y < vh) {
        y += self->rowHeight(a);
        if (!self->nextRow(a))
            break;
    }
    if (!(a == c))
        return QRect();
    BlockP b = self->block(c.block);
    QTextLine line = b->layout.lineAt(c.line);
    int idx = idxOf(*b, cursor_) + (preedit_.isEmpty() ? 0 : preeditCursor_);
    qreal x = line.cursorToX(idx);
    return QRect(int(kMargin + xShift() + x), int(y), 1, int(b->rows[c.line + 1] - b->rows[c.line]));
}

uint64_t Editor::hitTest(const QPoint &pt)
{
    BlockP b = block(top_.block);
    int topLine = qBound(0, top_.line, b->lineCount() - 1);
    qreal y = -b->rows[topLine];
    qreal py = qMax<qreal>(pt.y(), 0);
    for (;;) {
        if (py < y + b->height || b->last) {
            qreal ly = py - y;
            int n = b->lineCount();
            int i = (b->start == top_.block) ? topLine : 0;
            for (; i < n - 1; ++i) {
                if (ly < b->rows[i + 1])
                    break;
            }
            QTextLine l = b->layout.lineAt(i);
            int idx = l.xToCursor(pt.x() - kMargin - xShift());
            if (i < n - 1 && idx >= l.textStart() + l.textLength() && idx > l.textStart())
                idx = b->layout.previousCursorPosition(idx);
            return posOf(*b, idx);
        }
        y += b->height;
        b = block(b->next);
    }
}

// ---------------------------------------------------------------- cursor

uint64_t Editor::normalize(uint64_t pos) const
{
    const uint64_t size = doc_.size();
    if (pos > size)
        pos = size;
    for (int i = 0; i < 3 && pos > 0 && pos < size && (doc_.byteAt(pos) & 0xC0) == 0x80; ++i)
        --pos;
    if (pos > 0 && pos < size && doc_.byteAt(pos) == '\n' && doc_.byteAt(pos - 1) == '\r')
        --pos;
    return pos;
}

void Editor::setCursor(uint64_t pos, bool keepAnchor)
{
    cursor_ = normalize(pos);
    if (!keepAnchor)
        anchor_ = cursor_;
    prefX_ = -1;
    breakMerge_ = true;
    restartBlink();
    ensureCursorVisible();
}

void Editor::restartBlink()
{
    cursorOn_ = true;
    int t = QApplication::cursorFlashTime();
    if (t > 0)
        blink_.start(t / 2, this);
    else
        blink_.stop();
}

uint64_t Editor::moveVisual(uint64_t pos, bool right)
{
    BlockP b = blockAt(pos);
    int idx = idxOf(*b, pos);
    int n = right ? b->layout.rightCursorPosition(idx) : b->layout.leftCursorPosition(idx);
    if (n != idx)
        return posOf(*b, n);
    bool forward = right != rtl_;
    if (forward)
        return b->last ? pos : b->next;
    if (b->start == 0)
        return pos;
    BlockP pb = blockAt(b->start - 1);
    if (pb->end == b->start)  // forced break: step back inside previous block
        return posOf(*pb, pb->layout.previousCursorPosition(pb->text.size()));
    return pb->end;
}

uint64_t Editor::moveWord(uint64_t pos, bool forward)
{
    BlockP b = blockAt(pos);
    const QString &t = b->text;
    int idx = idxOf(*b, pos), n = t.size();
    if (forward) {
        if (idx >= n)
            return b->last ? pos : b->next;
        int i = idx, c = charClass(t[i]);
        if (c != 0)
            while (i < n && charClass(t[i]) == c)
                ++i;
        while (i < n && t[i].isSpace())
            ++i;
        return posOf(*b, i);
    }
    if (idx == 0) {
        if (b->start == 0)
            return 0;
        BlockP pb = blockAt(b->start - 1);
        if (pb->end == b->start)
            return moveWord(posOf(*pb, pb->layout.previousCursorPosition(pb->text.size())), false);
        return pb->end;
    }
    int i = idx;
    while (i > 0 && t[i - 1].isSpace())
        --i;
    if (i > 0) {
        int c = charClass(t[i - 1]);
        while (i > 0 && charClass(t[i - 1]) == c)
            --i;
    }
    return posOf(*b, i);
}

uint64_t Editor::moveVertical(uint64_t pos, int rows)
{
    BlockP b = blockAt(pos);
    int idx = idxOf(*b, pos);
    QTextLine l = b->layout.lineForTextPosition(idx);
    if (!l.isValid())
        l = b->layout.lineAt(b->lineCount() - 1);
    if (prefX_ < 0)
        prefX_ = l.cursorToX(idx);
    Anchor a{b->start, l.lineNumber()};
    int moved = 0;
    for (int i = 0; i < qAbs(rows); ++i) {
        if (!(rows > 0 ? nextRow(a) : prevRow(a)))
            break;
        ++moved;
    }
    if (moved == 0)
        return rows > 0 ? doc_.size() : 0;
    BlockP nb = block(a.block);
    QTextLine nl = nb->layout.lineAt(a.line);
    int ni = nl.xToCursor(prefX_);
    if (a.line < nb->lineCount() - 1 && ni >= nl.textStart() + nl.textLength() && ni > nl.textStart())
        ni = nb->layout.previousCursorPosition(ni);
    return posOf(*nb, ni);
}

uint64_t Editor::lineHome(uint64_t pos)
{
    BlockP b = blockAt(pos);
    QTextLine l = b->layout.lineForTextPosition(idxOf(*b, pos));
    if (!l.isValid())
        return pos;
    return posOf(*b, l.textStart());
}

uint64_t Editor::lineEnd(uint64_t pos)
{
    BlockP b = blockAt(pos);
    QTextLine l = b->layout.lineForTextPosition(idxOf(*b, pos));
    if (!l.isValid())
        return pos;
    int e = l.textStart() + l.textLength();
    if (l.lineNumber() < b->lineCount() - 1 && e > l.textStart())
        e = b->layout.previousCursorPosition(e);
    return posOf(*b, e);
}

uint64_t Editor::prevCodepoint(uint64_t pos) const
{
    if (pos == 0)
        return 0;
    if (pos >= 2 && doc_.byteAt(pos - 1) == '\n' && doc_.byteAt(pos - 2) == '\r')
        return pos - 2;
    uint64_t p = pos - 1;
    while (p > 0 && pos - p < 4 && (doc_.byteAt(p) & 0xC0) == 0x80)
        --p;
    return p;
}

uint64_t Editor::nextGrapheme(uint64_t pos)
{
    BlockP b = blockAt(pos);
    int idx = idxOf(*b, pos);
    if (idx < b->text.size())
        return posOf(*b, b->layout.nextCursorPosition(idx));
    return b->last ? pos : b->next;
}

void Editor::wordRange(uint64_t pos, uint64_t &a, uint64_t &e)
{
    BlockP b = blockAt(pos);
    const QString &t = b->text;
    int n = t.size(), idx = idxOf(*b, pos);
    if (n == 0) {
        a = e = pos;
        return;
    }
    int k = idx < n ? idx : n - 1;
    int c = charClass(t[k]);
    int i0 = k, i1 = k + 1;
    while (i0 > 0 && charClass(t[i0 - 1]) == c)
        --i0;
    while (i1 < n && charClass(t[i1]) == c)
        ++i1;
    a = posOf(*b, i0);
    e = posOf(*b, i1);
}

void Editor::blockRange(uint64_t pos, uint64_t &a, uint64_t &e)
{
    BlockP b = blockAt(pos);
    a = b->start;
    e = b->last ? b->end : b->next;
}

// ---------------------------------------------------------------- editing

void Editor::replaceRange(uint64_t a, uint64_t b, const std::string &text, Kind kind)
{
    if (a == b && text.empty())
        return;
    std::vector<Piece> np;
    if (!text.empty())
        np.push_back(doc_.addText(text));
    applyPieces(a, b - a, std::move(np), text.size(), kind, a + text.size());
}

void Editor::applyPieces(uint64_t pos, uint64_t oldLen, std::vector<Piece> newP, uint64_t newLen, Kind kind,
                         uint64_t cursorAfter)
{
    if (trackDir_) {
        countDirRange(pos, pos + oldLen, -1);
        countDir(newP, 1);
    }
    std::vector<Piece> oldP = doc_.replace(pos, oldLen, newP);

    bool merged = false;
    if (!breakMerge_ && !undo_.empty() && undo_.size() != cleanIndex_) {
        UndoOp &l = undo_.back();
        if (kind == Kind::Typing && l.kind == Kind::Typing && oldLen == 0 && l.oldLen == 0 &&
            l.pos + l.newLen == pos) {
            for (const Piece &p : newP) {
                if (!l.newP.empty() && l.newP.back().buf == p.buf && l.newP.back().off + l.newP.back().len == p.off)
                    l.newP.back().len += p.len;
                else
                    l.newP.push_back(p);
            }
            l.newLen += newLen;
            merged = true;
        } else if (kind == Kind::DelBack && l.kind == Kind::DelBack && newLen == 0 && l.newLen == 0 &&
                   pos + oldLen == l.pos) {
            oldP.insert(oldP.end(), l.oldP.begin(), l.oldP.end());
            l.oldP = std::move(oldP);
            l.pos = pos;
            l.oldLen += oldLen;
            merged = true;
        } else if (kind == Kind::DelFwd && l.kind == Kind::DelFwd && newLen == 0 && l.newLen == 0 &&
                   pos == l.pos) {
            l.oldP.insert(l.oldP.end(), oldP.begin(), oldP.end());
            l.oldLen += oldLen;
            merged = true;
        }
    }
    if (!merged) {
        if (cleanIndex_ > undo_.size())
            cleanIndex_ = size_t(-1);
        undo_.push_back(UndoOp{pos, oldLen, newLen, std::move(oldP), std::move(newP), anchor_, cursor_, kind});
    }
    redo_.clear();
    breakMerge_ = kind == Kind::Other;
    afterChange(pos, oldLen, newLen);
    cursor_ = anchor_ = normalize(cursorAfter);
    prefX_ = -1;
    restartBlink();
    ensureCursorVisible();
}

void Editor::afterChange(uint64_t pos, uint64_t oldLen, uint64_t newLen)
{
    cache_.clear();
    maxTopValid_ = false;
    if (top_.block > pos) {
        if (top_.block >= pos + oldLen)
            top_.block = top_.block - oldLen + newLen;
        else
            top_.block = pos;
    }
    if (trackDir_ && dirMode_ == 0)
        setDirection(strongR_ > strongL_);
    clampTop();
    emit modifiedChanged(isModified());
}

void Editor::typeText(const QString &s)
{
    std::string u = s.toUtf8().toStdString();
    Kind k = (hasSelection() || u.find('\n') != std::string::npos) ? Kind::Other : Kind::Typing;
    replaceRange(selStart(), selEnd(), u, k);
}

void Editor::undo()
{
    if (undo_.empty())
        return;
    UndoOp op = std::move(undo_.back());
    undo_.pop_back();
    if (trackDir_) {
        countDir(op.newP, -1);
        countDir(op.oldP, 1);
    }
    doc_.replace(op.pos, op.newLen, op.oldP);
    afterChange(op.pos, op.newLen, op.oldLen);
    anchor_ = normalize(op.selA);
    cursor_ = normalize(op.selC);
    redo_.push_back(std::move(op));
    breakMerge_ = true;
    prefX_ = -1;
    ensureCursorVisible();
    emit modifiedChanged(isModified());
}

void Editor::redo()
{
    if (redo_.empty())
        return;
    UndoOp op = std::move(redo_.back());
    redo_.pop_back();
    if (trackDir_) {
        countDir(op.oldP, -1);
        countDir(op.newP, 1);
    }
    doc_.replace(op.pos, op.oldLen, op.newP);
    undo_.push_back(std::move(op));
    const UndoOp &o = undo_.back();
    afterChange(o.pos, o.oldLen, o.newLen);
    cursor_ = anchor_ = normalize(o.pos + o.newLen);
    breakMerge_ = true;
    prefX_ = -1;
    ensureCursorVisible();
    emit modifiedChanged(isModified());
}

void Editor::copy()
{
    if (!hasSelection())
        return;
    if (selEnd() - selStart() > (uint64_t(1) << 30)) {
        QApplication::beep();
        return;
    }
    QGuiApplication::clipboard()->setText(QString::fromUtf8(doc_.text(selStart(), selEnd())));
}

void Editor::cut()
{
    if (!hasSelection() || selEnd() - selStart() > (uint64_t(1) << 30))
        return;
    copy();
    replaceRange(selStart(), selEnd(), std::string(), Kind::Other);
}

void Editor::paste(bool primary)
{
    QString t = QGuiApplication::clipboard()->text(primary ? QClipboard::Selection : QClipboard::Clipboard);
    if (t.isEmpty())
        return;
    t.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    if (eol_ == "\r\n")
        t.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    replaceRange(selStart(), selEnd(), t.toUtf8().toStdString(), Kind::Other);
}

void Editor::selectAll()
{
    anchor_ = 0;
    cursor_ = doc_.size();
    breakMerge_ = true;
    ensureCursorVisible();
    updatePrimary();
}

void Editor::updatePrimary()
{
    QClipboard *cb = QGuiApplication::clipboard();
    if (hasSelection() && selEnd() - selStart() < (1u << 20) && cb->supportsSelection())
        cb->setText(QString::fromUtf8(doc_.text(selStart(), selEnd())), QClipboard::Selection);
}

void Editor::deleteBack(bool word)
{
    if (hasSelection()) {
        replaceRange(selStart(), selEnd(), std::string(), Kind::Other);
        return;
    }
    if (cursor_ == 0)
        return;
    uint64_t a = word ? moveWord(cursor_, false) : prevCodepoint(cursor_);
    replaceRange(a, cursor_, std::string(), word ? Kind::Other : Kind::DelBack);
}

void Editor::deleteForward(bool word)
{
    if (hasSelection()) {
        replaceRange(selStart(), selEnd(), std::string(), Kind::Other);
        return;
    }
    uint64_t e = word ? moveWord(cursor_, true) : nextGrapheme(cursor_);
    if (e > cursor_)
        replaceRange(cursor_, e, std::string(), word ? Kind::Other : Kind::DelFwd);
}

// ---------------------------------------------------------------- search

bool Editor::find(Searcher &s, bool backward, bool &wrapped)
{
    wrapped = false;
    uint64_t ms, me;
    bool slow = doc_.size() > (64u << 20);
    if (slow)
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok;
    if (!backward) {
        uint64_t from = selEnd();
        ok = s.findForward(from, ms, me);
        if (ok && me == ms && ms == from && !hasSelection()) {  // don't get stuck on an empty match
            uint64_t n = nextGrapheme(from);
            ok = n > from && s.findForward(n, ms, me);
        }
        if (!ok) {
            ok = s.findForward(0, ms, me);
            wrapped = ok;
        }
    } else {
        ok = s.findBackward(selStart(), ms, me);
        if (!ok) {
            ok = s.findBackward(doc_.size() + 1, ms, me);
            wrapped = ok;
        }
    }
    if (slow)
        QGuiApplication::restoreOverrideCursor();
    if (!ok)
        return false;
    selectMatch(ms, me);
    return true;
}

void Editor::selectMatch(uint64_t ms, uint64_t me)
{
    anchor_ = ms;
    cursor_ = me;
    breakMerge_ = true;
    prefX_ = -1;
    restartBlink();
    ensureCursorVisible();
}

bool Editor::findFirst(Searcher &s, bool fromEnd)
{
    uint64_t ms, me;
    bool slow = doc_.size() > (64u << 20);
    if (slow)
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = fromEnd ? s.findBackward(doc_.size() + 1, ms, me) : s.findForward(0, ms, me);
    if (slow)
        QGuiApplication::restoreOverrideCursor();
    if (ok)
        selectMatch(ms, me);
    return ok;
}

bool Editor::replaceFirst(Searcher &s, const std::string &repl, bool fromEnd)
{
    if (!findFirst(s, fromEnd))
        return false;
    uint64_t ms = selStart(), me = selEnd();
    std::string out;
    if (!s.matchesExactly(ms, me, repl, out))
        out = repl;
    replaceRange(ms, me, out, Kind::Other);
    uint64_t a, b;
    if (fromEnd ? s.findBackward(ms, a, b) : s.findForward(ms + out.size(), a, b))
        selectMatch(a, b);
    return true;
}

bool Editor::replaceOne(Searcher &s, const std::string &repl, bool &wrapped)
{
    std::string out;
    if (hasSelection() && s.matchesExactly(selStart(), selEnd(), repl, out)) {
        replaceRange(selStart(), selEnd(), out, Kind::Other);
    }
    return find(s, false, wrapped);
}

uint64_t Editor::replaceAll(Searcher &s, const std::string &repl)
{
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const uint32_t kAcc = 0xFFFFFFFFu;  // pieces pointing into acc, fixed up later
    std::vector<Piece> all = doc_.allPieces();
    size_t pi = 0;
    uint64_t pstart = 0;
    std::vector<Piece> np;
    std::string acc;
    uint64_t prev = 0, count = 0;

    auto push = [&](Piece p) {
        if (!p.len)
            return;
        if (!np.empty() && np.back().buf == p.buf && np.back().off + np.back().len == p.off)
            np.back().len += p.len;
        else
            np.push_back(p);
    };
    auto pushAcc = [&](const char *d, size_t n) {
        Piece p{acc.size(), n, kAcc};
        acc.append(d, n);
        push(p);
    };
    auto copyRange = [&](uint64_t a, uint64_t b) {
        if (b - a < 256) {
            std::string t = doc_.text(a, b);
            pushAcc(t.data(), t.size());
            return;
        }
        while (pi < all.size() && pstart + all[pi].len <= a) {
            pstart += all[pi].len;
            ++pi;
        }
        while (a < b && pi < all.size()) {
            uint64_t k = a - pstart;
            uint64_t n = std::min(all[pi].len - k, b - a);
            push(Piece{all[pi].off + k, n, all[pi].buf});
            a += n;
            if (k + n == all[pi].len) {
                pstart += all[pi].len;
                ++pi;
            }
        }
    };
    s.forEachMatch(repl, [&](uint64_t ms, uint64_t me, std::string_view r) {
        copyRange(prev, ms);
        pushAcc(r.data(), r.size());
        prev = me;
        ++count;
    });
    if (count == 0) {
        QGuiApplication::restoreOverrideCursor();
        return 0;
    }
    copyRange(prev, doc_.size());
    Piece base = doc_.addOwned(std::move(acc));
    std::vector<Piece> fixed;
    fixed.reserve(np.size());
    for (Piece p : np) {
        if (p.buf == kAcc) {
            p.buf = base.buf;
            p.off += base.off;
        }
        if (!fixed.empty() && fixed.back().buf == p.buf && fixed.back().off + fixed.back().len == p.off)
            fixed.back().len += p.len;
        else
            fixed.push_back(p);
    }
    uint64_t newLen = Document::piecesLength(fixed);
    uint64_t c = std::min(cursor_, newLen);
    breakMerge_ = true;
    applyPieces(0, doc_.size(), std::move(fixed), newLen, Kind::Other, c);
    QGuiApplication::restoreOverrideCursor();
    return count;
}

// ---------------------------------------------------------------- input

bool Editor::event(QEvent *e)
{
    return QAbstractScrollArea::event(e);
}

void Editor::keyPressEvent(QKeyEvent *e)
{
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;

    if (e->matches(QKeySequence::Undo) && !shift) { undo(); return; }
    if (e->matches(QKeySequence::Redo) || (ctrl && e->key() == Qt::Key_Y) ||
        (ctrl && shift && e->key() == Qt::Key_Z)) { redo(); return; }
    if (e->matches(QKeySequence::Copy)) { copy(); return; }
    if (e->matches(QKeySequence::Cut)) { cut(); return; }
    if (e->matches(QKeySequence::Paste)) { paste(); return; }
    if (e->matches(QKeySequence::SelectAll)) { selectAll(); return; }

    auto move = [&](uint64_t pos) {
        setCursor(pos, shift);
        if (shift)
            updatePrimary();
    };
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        bool right = e->key() == Qt::Key_Right;
        if (hasSelection() && !shift) {
            move((right != rtl_) ? selEnd() : selStart());
            return;
        }
        move(ctrl ? moveWord(cursor_, right != rtl_) : moveVisual(cursor_, right));
        return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        int d = e->key() == Qt::Key_Down ? 1 : -1;
        if (ctrl) {
            scrollRows(d);
            return;
        }
        uint64_t p = moveVertical(cursor_, d);
        qreal px = prefX_;
        setCursor(p, shift);
        prefX_ = px;
        if (shift)
            updatePrimary();
        return;
    }
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        int n = rowsPerPage() * (e->key() == Qt::Key_PageDown ? 1 : -1);
        uint64_t p = moveVertical(cursor_, n);
        qreal px = prefX_;
        scrollRows(n);
        setCursor(p, shift);
        prefX_ = px;
        return;
    }
    case Qt::Key_Home:
        move(ctrl ? 0 : lineHome(cursor_));
        return;
    case Qt::Key_End:
        move(ctrl ? doc_.size() : lineEnd(cursor_));
        return;
    case Qt::Key_Backspace:
        deleteBack(ctrl);
        return;
    case Qt::Key_Delete:
        if (shift && !ctrl) { cut(); return; }
        deleteForward(ctrl);
        return;
    case Qt::Key_Insert:
        if (ctrl) copy();
        else if (shift) paste();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        replaceRange(selStart(), selEnd(), eol_, Kind::Other);
        return;
    case Qt::Key_Tab:
        if (!ctrl) typeText(QStringLiteral("\t"));
        return;
    case Qt::Key_Backtab:
        return;
    case Qt::Key_Escape:
        if (hasSelection())
            setCursor(cursor_, false);
        e->ignore();
        return;
    default:
        break;
    }
    // Plain text, or Ctrl+Alt (AltGr on some setups); not Ctrl-only or Alt-only shortcuts.
    QString t = e->text();
    const bool alt = e->modifiers() & Qt::AltModifier;
    if (!t.isEmpty() && ctrl == alt) {
        bool printable = true;
        for (QChar c : t)
            if (c.category() == QChar::Other_Control)
                printable = false;
        if (printable) {
            typeText(t);
            return;
        }
    }
    e->ignore();
}

void Editor::inputMethodEvent(QInputMethodEvent *e)
{
    if (!e->commitString().isEmpty() || e->replacementLength() > 0) {
        if (e->replacementLength() > 0) {
            BlockP b = blockAt(cursor_);
            int i = idxOf(*b, cursor_);
            int s = qBound(0, i + e->replacementStart(), int(b->text.size()));
            int en = qBound(s, s + e->replacementLength(), int(b->text.size()));
            anchor_ = posOf(*b, s);
            cursor_ = posOf(*b, en);
        }
        preedit_.clear();
        if (!e->commitString().isEmpty())
            typeText(e->commitString());
        else
            replaceRange(selStart(), selEnd(), std::string(), Kind::Other);
    }
    QString old = preedit_;
    preedit_ = e->preeditString();
    preeditAttrs_ = e->attributes();
    preeditCursor_ = preedit_.size();
    for (const auto &a : preeditAttrs_)
        if (a.type == QInputMethodEvent::Cursor)
            preeditCursor_ = a.start;
    if (!preedit_.isEmpty() || !old.isEmpty()) {
        cache_.erase(blockStartOf(cursor_));
        maxTopValid_ = false;
        ensureCursorVisible();
    }
    e->accept();
}

QVariant Editor::inputMethodQuery(Qt::InputMethodQuery q) const
{
    Editor *self = const_cast<Editor *>(this);
    switch (q) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImHints:
        return int(Qt::ImhMultiLine | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    case Qt::ImCursorRectangle:
        return cursorRect();
    case Qt::ImFont:
        return font_;
    case Qt::ImCursorPosition: {
        BlockP b = self->blockAt(cursor_);
        return idxOf(*b, cursor_);
    }
    case Qt::ImAnchorPosition: {
        BlockP b = self->blockAt(cursor_);
        uint64_t a = (anchor_ >= b->start && anchor_ <= b->end) ? anchor_ : cursor_;
        return idxOf(*b, a);
    }
    case Qt::ImSurroundingText:
        return self->blockAt(cursor_)->text;
    case Qt::ImCurrentSelection:
        return selectedText(4096);
    default:
        return QAbstractScrollArea::inputMethodQuery(q);
    }
}

void Editor::mousePressEvent(QMouseEvent *e)
{
    if (!preedit_.isEmpty())
        QGuiApplication::inputMethod()->commit();
    QPoint pt = e->position().toPoint();
    if (e->button() == Qt::LeftButton) {
        uint64_t pos = hitTest(pt);
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!(e->modifiers() & Qt::ShiftModifier) && now - lastDblClickMs_ < QApplication::doubleClickInterval() &&
            (pt - lastDblClickPt_).manhattanLength() < 6) {
            blockRange(pos, initA_, initB_);
            setCursor(initB_, false);
            anchor_ = initA_;
            clickMode_ = 3;
        } else {
            setCursor(pos, e->modifiers() & Qt::ShiftModifier);
            clickMode_ = 1;
        }
        selecting_ = true;
        viewport()->update();
    } else if (e->button() == Qt::MiddleButton) {
        setCursor(hitTest(pt), false);
        paste(true);
    }
}

void Editor::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    QPoint pt = e->position().toPoint();
    wordRange(hitTest(pt), initA_, initB_);
    setCursor(initB_, false);
    anchor_ = initA_;
    clickMode_ = 2;
    selecting_ = true;
    lastDblClickMs_ = QDateTime::currentMSecsSinceEpoch();
    lastDblClickPt_ = pt;
    viewport()->update();
}

void Editor::mouseMoveEvent(QMouseEvent *e)
{
    if (!selecting_ || !(e->buttons() & Qt::LeftButton))
        return;
    lastMouse_ = e->position().toPoint();
    int h = viewport()->height();
    if (lastMouse_.y() < 0 || lastMouse_.y() >= h) {
        autoScrollDir_ = lastMouse_.y() < 0 ? -1 : 1;
        if (!autoScroll_.isActive())
            autoScroll_.start(40, this);
    } else {
        autoScroll_.stop();
    }
    QPoint p(lastMouse_.x(), qBound(0, lastMouse_.y(), h - 1));
    uint64_t pos = hitTest(p);
    if (clickMode_ == 1) {
        cursor_ = normalize(pos);
    } else {
        uint64_t a, b;
        if (clickMode_ == 2)
            wordRange(pos, a, b);
        else
            blockRange(pos, a, b);
        if (a < initA_) {
            anchor_ = initB_;
            cursor_ = a;
        } else {
            anchor_ = initA_;
            cursor_ = std::max(b, initB_);
        }
    }
    restartBlink();
    if (!wrap_ && (lastMouse_.x() < 0 || lastMouse_.x() > viewport()->width()))
        ensureCursorVisible();
    viewport()->update();
}

void Editor::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        selecting_ = false;
        autoScroll_.stop();
        updatePrimary();
    }
}

void Editor::wheelEvent(QWheelEvent *e)
{
    int dy = e->angleDelta().y();
    if (e->modifiers() & Qt::ControlModifier) {
        wheelAccum_ += dy;
        int steps = wheelAccum_ / 120;
        wheelAccum_ -= steps * 120;
        if (steps)
            setZoom(zoom_ + steps * 10);
        e->accept();
        return;
    }
    int dx = e->angleDelta().x();
    if (e->modifiers() & Qt::ShiftModifier)
        std::swap(dx, dy);
    if (dx && !wrap_) {
        QScrollBar *hb = horizontalScrollBar();
        hb->setValue(hb->value() - (rtl_ ? -1 : 1) * dx * hb->singleStep() / 40);
        e->accept();
        return;
    }
    wheelAccum_ += dy;
    int rows = wheelAccum_ / 40;
    wheelAccum_ -= rows * 40;
    if (rows)
        scrollRows(-rows);
    e->accept();
}

void Editor::focusInEvent(QFocusEvent *e)
{
    restartBlink();
    viewport()->update();
    QAbstractScrollArea::focusInEvent(e);
}

void Editor::focusOutEvent(QFocusEvent *e)
{
    blink_.stop();
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(e);
}

void Editor::timerEvent(QTimerEvent *e)
{
    if (e->timerId() == blink_.timerId()) {
        cursorOn_ = !cursorOn_;
        QRect r = cursorRect();
        if (!r.isNull())
            viewport()->update(r.adjusted(-2, 0, 2, 0));
    } else if (e->timerId() == autoScroll_.timerId()) {
        int h = viewport()->height();
        int dist = autoScrollDir_ < 0 ? -lastMouse_.y() : lastMouse_.y() - h;
        scrollRows(autoScrollDir_ * qBound(1, dist / 20 + 1, 20));
        QPoint p(lastMouse_.x(), autoScrollDir_ < 0 ? 0 : h - 1);
        uint64_t pos = hitTest(p);
        if (clickMode_ == 1)
            cursor_ = normalize(pos);
        else
            cursor_ = pos;
        viewport()->update();
    } else {
        QAbstractScrollArea::timerEvent(e);
    }
}

void Editor::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu m(this);
    auto add = [&](const QString &text, const QKeySequence &key, auto fn, bool enabled) {
        QAction *a = m.addAction(text, this, fn);
        a->setShortcut(key);
        a->setEnabled(enabled);
        return a;
    };
    add(tr("Undo"), QKeySequence::Undo, [this] { undo(); }, !undo_.empty());
    add(tr("Redo"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this] { redo(); }, !redo_.empty());
    m.addSeparator();
    add(tr("Cut"), QKeySequence::Cut, [this] { cut(); }, hasSelection());
    add(tr("Copy"), QKeySequence::Copy, [this] { copy(); }, hasSelection());
    add(tr("Paste"), QKeySequence::Paste, [this] { paste(); }, true);
    add(tr("Delete"), QKeySequence::Delete, [this] { deleteForward(false); }, hasSelection());
    m.addSeparator();
    add(tr("Select All"), QKeySequence::SelectAll, [this] { selectAll(); }, true);
    emit contextMenuAboutToShow(&m);
    m.exec(e->globalPos());
}

void Editor::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void Editor::dragMoveEvent(QDragMoveEvent *e)
{
    e->acceptProposedAction();
}

void Editor::dropEvent(QDropEvent *e)
{
    const QMimeData *md = e->mimeData();
    if (md->hasUrls()) {
        for (const QUrl &u : md->urls()) {
            if (u.isLocalFile()) {
                e->acceptProposedAction();
                emit fileDropped(u.toLocalFile());
                return;
            }
        }
    }
    if (md->hasText()) {
        setCursor(hitTest(e->position().toPoint()), false);
        replaceRange(cursor_, cursor_, md->text().toUtf8().toStdString(), Kind::Other);
        e->acceptProposedAction();
    }
}
