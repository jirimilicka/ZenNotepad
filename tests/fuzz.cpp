#include "../src/mainwindow.h"
#include "../src/search.h"
#include <QApplication>
#include <QTest>
#include <QScrollBar>
#include <QFile>
#include <QTemporaryDir>
#include <random>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); if (fails > 10) exit(1);} } while (0)

struct EditorTest {
    static uint64_t cur(Editor &e) { return e.cursor_; }
    static void shot(const QString &file, const QString &outDir)
    {
        QApplication::setStyle(QStringLiteral("breeze"));
        setUiLanguage(QStringLiteral("en"));
        {
            MainWindow w;
            w.resize(860, 520);
            w.openPath(file);
            w.editor_->setZoom(120);
            w.scaleBar(120);
            w.setInverted(true);
            w.show();
            w.find_->setText(QStringLiteral("kůň"));
            w.showBar(true);
            w.repl_->setText(QStringLiteral("horse"));
            w.findNext(false);
            QTest::qWait(200);
            w.grab().save(outDir + "/find-dark.png");
            {
            MainWindow w;
            w.resize(860, 520);
            w.openPath(file);
            w.editor_->setZoom(120);
            w.scaleBar(120);
            w.show();
            w.editor_->setFocus();
            QTest::qWait(200);
            w.grab().save(outDir + "/main.png");
        }
    }
    }
    static void blocks(Editor &e, std::mt19937 &rng)
    {
        uint64_t size = e.doc_.size(), s = 0, n = 0;
        std::vector<uint64_t> starts;
        for (;;) {
            uint64_t end, next; bool last;
            e.blockBounds(s, end, next, last);
            CHECK(end >= s && next >= end);
            CHECK(end - s <= 2 * 32768 + 1024);
            starts.push_back(s);
            if (last) { CHECK(end == size); break; }
            CHECK(next > s || (next == s && false));
            s = next;
            if (++n > 10000000) { CHECK(false); break; }
        }
        for (int i = 0; i < 2000; ++i) {
            uint64_t p = size ? rng() % (size + 1) : 0;
            auto it = std::upper_bound(starts.begin(), starts.end(), p) - 1;
            CHECK(e.blockStartOf(p) == *it);
        }
        for (uint64_t st : starts) CHECK(e.blockStartOf(st) == st);
    }
};

struct Dummy;
static std::string all(const Editor &e) { return e.document().text(0, e.document().size()); }

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc > 2) { EditorTest::shot(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2])); return 0; }
    std::mt19937 rng(42);
    QTemporaryDir dir;

    // 1. Document piece tree vs std::string model
    {
        Document d;
        std::string model;
        std::vector<std::tuple<uint64_t, std::vector<Piece>, uint64_t>> hist;
        for (int it = 0; it < 200000; ++it) {
            uint64_t sz = model.size();
            uint64_t pos = sz ? rng() % (sz + 1) : 0;
            uint64_t len = (rng() % 4 == 0 && sz > pos) ? rng() % std::min<uint64_t>(sz - pos, 50) : 0;
            std::string ins;
            int k = rng() % 6;
            for (int i = 0; i < k; ++i) ins += char('a' + rng() % 26);
            std::vector<Piece> np;
            if (!ins.empty()) np.push_back(d.addText(ins));
            auto old = d.replace(pos, len, np);
            CHECK(Document::piecesLength(old) == len);
            model.replace(pos, len, ins);
            hist.emplace_back(pos, old, ins.size());
            if (it % 997 == 0) CHECK(d.text(0, d.size()) == model);
        }
        CHECK(d.text(0, d.size()) == model);
        // undo everything
        while (!hist.empty()) {
            auto &[pos, old, nl] = hist.back();
            d.replace(pos, nl, old);
            hist.pop_back();
        }
        CHECK(d.size() == 0);
        fprintf(stderr, "piece tree fuzz done\n");
    }

    // 2. Searcher vs naive search
    {
        Document d;
        std::string model;
        for (int i = 0; i < 300; ++i) {
            std::string s;
            int k = rng() % 30;
            for (int j = 0; j < k; ++j) s += "ab\n "[rng() % 4];
            std::vector<Piece> np{d.addText(s)};
            uint64_t pos = model.empty() ? 0 : rng() % (model.size() + 1);
            d.replace(pos, 0, np);
            model.insert(pos, s);
        }
        Searcher se(d);
        std::string err;
        for (const char *pat : {"ab", "a", "b\na", "aba"}) {
            se.setPattern(pat, true, false, err);
            std::vector<uint64_t> got, exp;
            se.forEachMatch("", [&](uint64_t a, uint64_t, std::string_view) { got.push_back(a); });
            for (size_t p = model.find(pat); p != std::string::npos; p = model.find(pat, p + strlen(pat))) exp.push_back(p);
            CHECK(got == exp);
            uint64_t ms, me, from = 0; size_t i = 0;
            std::vector<uint64_t> fw;
            while (se.findForward(from, ms, me)) { fw.push_back(ms); from = ms + 1; if (++i > 100000) break; }
            std::vector<uint64_t> ov;
            for (size_t p = model.find(pat); p != std::string::npos; p = model.find(pat, p + 1)) ov.push_back(p);
            CHECK(fw == ov);
            std::vector<uint64_t> bw;
            uint64_t before = d.size() + 1;
            while (se.findBackward(before, ms, me)) { bw.push_back(ms); before = ms; }
            std::reverse(bw.begin(), bw.end());
            CHECK(bw == ov);
        }
        fprintf(stderr, "search fuzz done\n");
    }

    // 3. Block partition with long lines
    {
        Editor e;
        e.resize(500, 300);
        e.show();
        for (int round = 0; round < 6; ++round) {
            QString p = dir.filePath(QString("b%1.txt").arg(round));
            QFile f(p);
            if (!f.open(QIODevice::WriteOnly)) return 2;
            std::string s;
            int lines = 1 + rng() % 8;
            for (int l = 0; l < lines; ++l) {
                size_t len = (rng() % 3 == 0) ? rng() % 200000 : rng() % 100;
                for (size_t i = 0; i < len; ++i) {
                    int r = rng() % (round % 2 ? 1000 : 10);
                    s += r == 0 ? " " : (r < 3 ? "\xc5\xbe" : "x");  // spaces rarer in odd rounds
                }
                s += (rng() % 2) ? "\r\n" : "\n";
            }
            f.write(s.data(), s.size());
            f.close();
            QString err;
            CHECK(e.openFile(p, err));
            EditorTest::blocks(e, rng);
            // random scroll positions & painting
            for (int i = 0; i < 50; ++i) {
                e.verticalScrollBar()->setValue(rng() % (e.verticalScrollBar()->maximum() + 1));
                e.repaint();
            }
        }
        fprintf(stderr, "block fuzz done\n");
    }

    // 4. Editor random keys, then full undo/redo
    {
        Editor e;
        e.resize(400, 300);
        e.show();
        e.setFocus();
        QString p = dir.filePath("k.txt");
        { QFile f(p); if (!f.open(QIODevice::WriteOnly)) return 2; f.write("Příliš žluťoučký kůň\r\núpěl ďábelské ódy.\nשלום abc\n"); }
        QString err;
        CHECK(e.openFile(p, err));
        std::string orig = all(e);
        const Qt::Key keys[] = {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down, Qt::Key_Home, Qt::Key_End,
                                Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Return, Qt::Key_PageDown, Qt::Key_PageUp};
        const char *texts[] = {"a", "ř", "ש", "x", " ", "\t", "😀"};
        for (int i = 0; i < 20000; ++i) {
            int r = rng() % 40; if (r >= 20) r = 11 + rng() % 7;
            Qt::KeyboardModifiers m = Qt::NoModifier;
            if (rng() % 4 == 0) m |= Qt::ShiftModifier;
            if (rng() % 6 == 0) m |= Qt::ControlModifier;
            if (r < 11) {
                QTest::keyClick(&e, keys[r], m);
            } else if (r < 18) {
                QString t = QString::fromUtf8(texts[r - 11]);
                QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, t);
                QApplication::sendEvent(&e, &ev);
            } else if (r == 18) {
                QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier);
            } else {
                QTest::keyClick(&e, Qt::Key_Y, Qt::ControlModifier);
            }
            // invariant: valid UTF-8 never broken (we only insert valid text)
            if (i % 500 == 0) {
                QByteArray b = QByteArray::fromStdString(all(e));
                CHECK(QString::fromUtf8(b).toUtf8() == b);
            }
        }
        std::string fin = all(e);
        for (int i = 0; i < 100000; ++i) QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier);
        CHECK(all(e) == orig);
        CHECK(!e.isModified());
        for (int i = 0; i < 100000; ++i) QTest::keyClick(&e, Qt::Key_Y, Qt::ControlModifier);
        CHECK(all(e) == fin);
        fprintf(stderr, "editor key fuzz done (%zu bytes)\n", fin.size());
        Searcher se(e.document());
        std::string er;
        for (const char *pat : {"^", "x*", "$", "\\b"}) {
            CHECK(se.setPattern(pat, true, true, er));
            uint64_t prev = ~0ull; int moves = 0;
            for (int i = 0; i < 50; ++i) { bool w; e.find(se, false, w); uint64_t c = EditorTest::cur(e); if (c != prev) ++moves; prev = c; }
            CHECK(moves > 1);
            e.replaceAll(se, "");
        }
    }
    // 5. No-wrap mode: cursor stays visible horizontally, LTR and RTL
    {
        Editor e;
        e.resize(400, 200);
        e.show();
        e.setFocus();
        e.setWordWrap(false);
        for (const char *line : {"abc def ghi jkl mno pqr stu vwx yz ", "שלום עולם מה שלומך היום חבר טוב "}) {
            QString p = dir.filePath("nw.txt");
            { QFile f(p); if (!f.open(QIODevice::WriteOnly)) return 2; for (int i = 0; i < 40; ++i) f.write(line); f.write("\nshort\n"); }
            QString err;
            CHECK(e.openFile(p, err));
            CHECK(static_cast<QWidget &>(e).inputMethodQuery(Qt::ImCursorRectangle).toRect().x() >= 0);
            QTest::keyClick(&e, Qt::Key_End);
            QRect r = static_cast<QWidget &>(e).inputMethodQuery(Qt::ImCursorRectangle).toRect();
            CHECK(r.x() >= 0 && r.x() <= e.viewport()->width());
            CHECK(e.horizontalScrollBar()->maximum() > 0);
            QTest::keyClick(&e, Qt::Key_Home);
            r = static_cast<QWidget &>(e).inputMethodQuery(Qt::ImCursorRectangle).toRect();
            CHECK(r.x() >= 0 && r.x() <= e.viewport()->width());
            for (int i = 0; i < 300; ++i) QTest::keyClick(&e, Qt::Key_Right);
            r = static_cast<QWidget &>(e).inputMethodQuery(Qt::ImCursorRectangle).toRect();
            CHECK(r.x() >= 0 && r.x() <= e.viewport()->width());
            e.grab().save(dir.filePath(QString("nw%1.png").arg(int(line[0] != 'a'))));
        }
        // vertical scrollbar must not jump: viewport height stays stable while scrolling
        {
            QString p = dir.filePath("mixed.txt");
            QFile f(p);
            if (!f.open(QIODevice::WriteOnly)) return 2;
            for (int i = 0; i < 2000; ++i) {
                if (i % 97 == 50) for (int k = 0; k < 60; ++k) f.write("long line text ");
                else f.write("short");
                f.write("\n");
            }
            f.close();
            QString err;
            CHECK(e.openFile(p, err));
            int heightChanges = 0, h = e.viewport()->height(), backwards = 0, prev = 0;
            for (int i = 0; i < 300; ++i) {
                QWheelEvent we(QPointF(50, 50), e.mapToGlobal(QPointF(50, 50)), QPoint(), QPoint(0, -120),
                               Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(e.viewport(), &we);
                QApplication::processEvents();
                if (e.viewport()->height() != h) { ++heightChanges; h = e.viewport()->height(); }
                int v = e.verticalScrollBar()->value();
                if (v < prev) ++backwards;
                prev = v;
            }
            fprintf(stderr, "nowrap scroll: viewport height changes=%d, scrollbar moved backwards=%d\n", heightChanges, backwards);
            CHECK(heightChanges <= 1);
            CHECK(backwards == 0);
        }
        e.setInverted(true);
        e.setWordWrap(true);
        e.grab().save(dir.filePath("inv.png"));
        fprintf(stderr, "nowrap done: %s\n", qPrintable(dir.path()));
        dir.setAutoRemove(false);
    }
    fprintf(stderr, fails ? "%d FAILURES\n" : "ALL OK\n", fails);
    return fails != 0;
}
