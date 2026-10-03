#pragma once
#include "document.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QFont>
#include <QInputMethodEvent>
#include <QTextLayout>
#include <memory>
#include <unordered_map>
#include <vector>

class Searcher;
class QMenu;

// A paragraph ("block") of the document: bytes [start,end) laid out with QTextLayout.
// Blocks end at '\n' (a preceding '\r' is excluded from the text) or, for absurdly long
// lines, at a deterministic forced break so that layout cost stays bounded.
struct Block {
    uint64_t start = 0, end = 0, next = 0;
    bool last = false;
    QString text;
    std::vector<uint32_t> u16;  // utf-16 index -> byte offset relative to start
    QTextLayout layout;
    qreal height = 0;
    std::vector<qreal> rows;  // top of each visual row, plus the bottom (size lineCount+1)
    int lineCount() const { return layout.lineCount(); }
};
using BlockP = std::shared_ptr<Block>;

class Editor : public QAbstractScrollArea {
    Q_OBJECT
    friend struct EditorTest;
public:
    explicit Editor(QWidget *parent = nullptr);
    ~Editor() override;

    bool openFile(const QString &path, QString &err);
    bool saveFile(const QString &path, QString &err);
    bool isModified() const { return undo_.size() != cleanIndex_; }
    bool isEmptyUntouched() const { return doc_.size() == 0 && undo_.empty() && redo_.empty(); }
    uint64_t documentSize() const { return doc_.size(); }
    uint64_t cursorPosition() const { return cursor_; }
    const Document &document() const { return doc_; }

    QString selectedText(uint64_t limit) const;
    void setZoom(int percent);
    int zoom() const { return zoom_; }
    void setWordWrap(bool on);
    bool wordWrap() const { return wrap_; }
    void setInverted(bool on);
    // 0 = automatic (by majority of strong characters), 1 = left to right, 2 = right to left
    void setDirectionMode(int mode);
    int directionMode() const { return dirMode_; }
    bool inverted() const { return inverted_; }

    // Search. Return false when nothing was found. wrapped is set if the search wrapped.
    bool find(Searcher &s, bool backward, bool &wrapped);
    bool replaceOne(Searcher &s, const std::string &repl, bool &wrapped);
    // First match from the start of the document (or the last one from the end).
    bool findFirst(Searcher &s, bool fromEnd);
    // Replaces the first match from the start (or the last from the end), then selects the
    // following (preceding) match, if any.
    bool replaceFirst(Searcher &s, const std::string &repl, bool fromEnd);
    uint64_t replaceAll(Searcher &s, const std::string &repl);

signals:
    void modifiedChanged(bool modified);
    void fileDropped(const QString &path);
    void zoomChanged(int percent);
    void contextMenuAboutToShow(QMenu *menu);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void inputMethodEvent(QInputMethodEvent *) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void timerEvent(QTimerEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dropEvent(QDropEvent *) override;
    bool focusNextPrevChild(bool) override { return false; }
    void scrollContentsBy(int, int) override {}
    bool event(QEvent *) override;

private:
    struct Anchor {
        uint64_t block = 0;
        int line = 0;
        bool operator<(const Anchor &o) const { return block < o.block || (block == o.block && line < o.line); }
        bool operator==(const Anchor &o) const { return block == o.block && line == o.line; }
    };
    enum class Kind { Other, Typing, DelBack, DelFwd };
    struct UndoOp {
        uint64_t pos, oldLen, newLen;
        std::vector<Piece> oldP, newP;
        uint64_t selA, selC;  // selection before the edit
        Kind kind;
    };

    // block geometry
    bool forcedBoundary(uint64_t k, uint64_t &b) const;
    uint64_t blockStartOf(uint64_t p) const;
    void blockBounds(uint64_t s, uint64_t &end, uint64_t &next, bool &last) const;
    BlockP block(uint64_t start);
    BlockP blockAt(uint64_t pos) { return block(blockStartOf(pos)); }
    int idxOf(const Block &b, uint64_t pos) const;
    uint64_t posOf(const Block &b, int idx) const { return b.start + b.u16[idx]; }
    void invalidateLayout();

    // rows
    bool nextRow(Anchor &a);
    bool prevRow(Anchor &a);
    qreal rowHeight(const Anchor &a);
    Anchor anchorOf(uint64_t pos);
    Anchor maxTop();
    void clampTop();
    void ensureCursorVisible();
    void scrollRows(int n);
    int rowsPerPage();
    void updateScrollBar();
    void updateHScroll();
    qreal xShift() const { return wrap_ ? 0 : (rtl_ ? xOff_ : -xOff_); }
    void scrollToValue(int v);
    uint64_t hitTest(const QPoint &pt);
    QRect cursorRect() const;

    // cursor movement
    void setCursor(uint64_t pos, bool keepAnchor);
    void selectMatch(uint64_t ms, uint64_t me);
    uint64_t normalize(uint64_t pos) const;
    uint64_t moveVisual(uint64_t pos, bool right);
    uint64_t moveWord(uint64_t pos, bool forward);
    uint64_t moveVertical(uint64_t pos, int rows);
    uint64_t lineHome(uint64_t pos);
    uint64_t lineEnd(uint64_t pos);
    uint64_t prevCodepoint(uint64_t pos) const;
    uint64_t nextGrapheme(uint64_t pos);
    void wordRange(uint64_t pos, uint64_t &a, uint64_t &b);
    void blockRange(uint64_t pos, uint64_t &a, uint64_t &b);
    uint64_t selStart() const { return std::min(anchor_, cursor_); }
    uint64_t selEnd() const { return std::max(anchor_, cursor_); }
    bool hasSelection() const { return anchor_ != cursor_; }

    // editing
    void replaceRange(uint64_t a, uint64_t b, const std::string &text, Kind kind);
    void applyPieces(uint64_t pos, uint64_t oldLen, std::vector<Piece> newP, uint64_t newLen, Kind kind,
                     uint64_t cursorAfter);
    void afterChange(uint64_t pos, uint64_t oldLen, uint64_t newLen);
    void typeText(const QString &s);
    void undo();
    void redo();
    void copy();
    void cut();
    void paste(bool primary = false);
    void selectAll();
    void deleteBack(bool word);
    void deleteForward(bool word);
    void setDirection(bool rtl);
    void countDir(const std::vector<Piece> &ps, int sign);
    void countDir(const char *p, uint64_t n, int sign);
    void countDirRange(uint64_t a, uint64_t b, int sign);
    int sbValueOf(const Anchor &a);
    void resetState();
    void updatePrimary();
    void restartBlink();

    Document doc_;
    uint64_t cursor_ = 0, anchor_ = 0;
    qreal prefX_ = -1;
    Anchor top_;
    Anchor maxTop_;
    bool maxTopValid_ = false;
    std::unordered_map<uint64_t, BlockP> cache_;
    QTextOption opt_;
    QFont font_;
    qreal basePointSize_ = 11;
    int zoom_ = 100;
    bool wrap_ = true;
    bool inverted_ = false;
    int dirMode_ = 0;
    int xOff_ = 0;  // horizontal scroll in no-wrap mode
    int hRange_ = 0;  // horizontal scroll range; only grows while scrolling (see updateHScroll)
    bool rtl_ = false;
    bool trackDir_ = true;
    int64_t strongL_ = 0, strongR_ = 0;
    std::string eol_ = "\n";

    std::vector<UndoOp> undo_, redo_;
    size_t cleanIndex_ = 0;
    bool breakMerge_ = true;

    QString preedit_;
    int preeditCursor_ = 0;
    QList<QInputMethodEvent::Attribute> preeditAttrs_;

    QBasicTimer blink_, autoScroll_;
    bool cursorOn_ = true;
    int autoScrollDir_ = 0;
    QPoint lastMouse_;
    bool selecting_ = false;
    int clickMode_ = 0;  // 1 char, 2 word, 3 line
    uint64_t initA_ = 0, initB_ = 0;  // initial word/line range for drag selection
    qint64 lastDblClickMs_ = 0;
    QPoint lastDblClickPt_;
    int wheelAccum_ = 0;
    int sbLastSet_ = -1;
    bool updatingSb_ = false;
    uint64_t pageBytes_ = 4096;
    uint64_t sbUnit_ = 1;
    static const int kMargin = 6;
};
