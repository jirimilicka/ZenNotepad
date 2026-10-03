// Headless tests: drives the Editor with synthetic input and checks document contents.
#include "../src/editor.h"
#include "../src/search.h"
#include <QApplication>
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void typ(QWidget *w, const QString &t)
{
    for (QChar c : t) {
        QKeyEvent ev(QEvent::KeyPress, c.unicode() < 128 ? int(c.toUpper().unicode()) : 0, Qt::NoModifier, QString(c));
        QApplication::sendEvent(w, &ev);
    }
}
static std::string all(const Editor &e) { return e.document().text(0, e.document().size()); }

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir dir;
    Editor e;
    e.resize(600, 400);
    e.show();
    e.setFocus();
    typ(&e, "hello world");
    CHECK(all(e) == "hello world");
    QTest::keyClick(&e, Qt::Key_Backspace);
    CHECK(all(e) == "hello worl");
    QTest::keyClick(&e, Qt::Key_Home);
    typ(&e, "X");
    CHECK(all(e) == "Xhello worl");
    QTest::keyClick(&e, Qt::Key_End);
    QTest::keyClick(&e, Qt::Key_Return);
    typ(&e, QString::fromUtf8("příliš žluťoučký"));
    CHECK(all(e) == "Xhello worl\npříliš žluťoučký");
    QTest::keyClick(&e, Qt::Key_Backspace);
    CHECK(all(e) == "Xhello worl\npříliš žluťoučk");
    QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier);
    CHECK(all(e) == "Xhello worl\npříliš žluťoučký");
    QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier);
    CHECK(all(e) == "Xhello worl\n");
    QTest::keyClick(&e, Qt::Key_Y, Qt::ControlModifier);
    CHECK(all(e) == "Xhello worl\npříliš žluťoučký");
    QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(all(e) == "Xhello worl\npříliš žluťoučký");
    e.setDirectionMode(2);
    typ(&e, "z");
    CHECK(e.directionMode() == 2);
    e.setDirectionMode(0);
    QTest::keyClick(&e, Qt::Key_Backspace);
    QTest::keyClick(&e, Qt::Key_Up);
    QTest::keyClick(&e, Qt::Key_Home);
    QTest::keyClick(&e, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(&e, Qt::Key_Delete);
    CHECK(all(e) == "hello worl\npříliš žluťoučký");
    QTest::keyClick(&e, Qt::Key_End, Qt::ControlModifier);
    QTest::keyClick(&e, Qt::Key_Left);
    QTest::keyClick(&e, Qt::Key_Backspace);
    CHECK(all(e) == "hello worl\npříliš žluťoučý");

    // search & replace
    {
        Searcher s(e.document());
        std::string err;
        CHECK(s.setPattern("o", true, false, err));
        CHECK(e.replaceAll(s, "0") == 3);
        CHECK(all(e) == "hell0 w0rl\npříliš žluť0učý");
        QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier);
        CHECK(all(e) == "hello worl\npříliš žluťoučý");
        CHECK(s.setPattern(QString::fromUtf8("ŽLUŤ").toUtf8().toStdString(), false, false, err));
        bool w;
        CHECK(e.find(s, false, w));
        CHECK(e.selectedText(100) == QString::fromUtf8("žluť"));
        CHECK(s.setPattern("(\\w+) (\\w+)", true, true, err));
        CHECK(e.replaceAll(s, "$2 $1") == 2);
        CHECK(all(e) == "worl hello\nžluťoučý příliš");
        CHECK(s.setPattern("^", true, true, err));
        CHECK(e.replaceAll(s, "> ") == 2);
        CHECK(all(e) == "> worl hello\n> žluťoučý příliš");
    }

    // find first / last, replace first / last
    {
        QString p = dir.filePath("ff.txt");
        { QFile f(p); if (!f.open(QIODevice::WriteOnly)) return 2; f.write("cat dog cat dog cat"); }
        QString err;
        CHECK(e.openFile(p, err));
        Searcher s(e.document());
        std::string er;
        CHECK(s.setPattern("cat", true, false, er));
        QTest::keyClick(&e, Qt::Key_Right);  // cursor after the first match start
        for (int i = 0; i < 9; ++i) QTest::keyClick(&e, Qt::Key_Right);
        CHECK(e.findFirst(s, false));
        CHECK(e.selectedText(100) == "cat" && e.cursorPosition() == 3);
        CHECK(e.findFirst(s, true));
        CHECK(e.cursorPosition() == 19);
        CHECK(e.replaceFirst(s, "CAT", false));
        CHECK(all(e) == "CAT dog cat dog cat");
        CHECK(e.cursorPosition() == 11);  // next match selected
        CHECK(e.replaceFirst(s, "X", true));
        CHECK(all(e) == "CAT dog cat dog X");
        CHECK(e.cursorPosition() == 11);  // previous match selected
        CHECK(e.replaceFirst(s, "c", false));
        CHECK(all(e) == "CAT dog c dog X");
        CHECK(!e.replaceFirst(s, "c", false));
    }

    // CRLF file, save, reopen
    QString p = dir.filePath("crlf.txt");
    { QFile f(p); f.open(QIODevice::WriteOnly); f.write("ab\r\ncd\r\n"); }
    QString err;
    CHECK(e.openFile(p, err));
    QTest::keyClick(&e, Qt::Key_End);
    QTest::keyClick(&e, Qt::Key_Return);
    typ(&e, "x");
    CHECK(all(e) == "ab\r\nx\r\ncd\r\n");
    QTest::keyClick(&e, Qt::Key_Down);
    QTest::keyClick(&e, Qt::Key_End);
    QTest::keyClick(&e, Qt::Key_Delete);
    CHECK(all(e) == "ab\r\nx\r\ncd");
    QTest::keyClick(&e, Qt::Key_Home);
    QTest::keyClick(&e, Qt::Key_Backspace);
    CHECK(all(e) == "ab\r\nxcd");
    CHECK(e.saveFile(p, err));
    { QFile f(p); f.open(QIODevice::ReadOnly); CHECK(f.readAll() == "ab\r\nxcd"); }
    CHECK(!e.isModified());
    QTest::keyClick(&e, Qt::Key_Z, Qt::ControlModifier);  // undo after save must still work
    CHECK(all(e) == "ab\r\nx\r\ncd");
    CHECK(e.isModified());

    // RTL detection
    p = dir.filePath("rtl.txt");
    { QFile f(p); f.open(QIODevice::WriteOnly); f.write(QString::fromUtf8("שלום עולם abc\nمرحبا بالعالم\n").toUtf8()); }
    CHECK(e.openFile(p, err));
    e.grab().save(dir.filePath("rtl.png"));

    // Huge single line: forced breaks must cover the document exactly
    p = dir.filePath("long.txt");
    {
        QFile f(p); f.open(QIODevice::WriteOnly);
        QByteArray line;
        for (int i = 0; i < 300000; ++i) line += (i % 7 == 0) ? "ž " : "abc ";
        f.write(line); f.write("\nshort\n"); f.write(line);
    }
    CHECK(e.openFile(p, err));
    QTest::keyClick(&e, Qt::Key_End, Qt::ControlModifier);
    typ(&e, "END");
    QTest::keyClick(&e, Qt::Key_Home, Qt::ControlModifier);
    typ(&e, "BEGIN");
    std::string s = all(e);
    CHECK(s.substr(0, 5) == "BEGIN" && s.substr(s.size() - 3) == "END");
    for (int i = 0; i < 200; ++i) QTest::keyClick(&e, Qt::Key_PageDown);
    for (int i = 0; i < 50; ++i) QTest::keyClick(&e, Qt::Key_Down);
    {
        QElapsedTimer tt; tt.start();
        typ(&e, "mid");
        for (int i = 0; i < 20; ++i) { typ(&e, "q"); e.repaint(); }
        fprintf(stderr, "typing in huge line: %.1f ms/key\n", tt.elapsed() / 23.0);
        e.document().size();
    }
    for (int i = 0; i < 20; ++i) QTest::keyClick(&e, Qt::Key_Backspace);
    CHECK(e.document().size() == s.size() + 3 + 20 - 20);

    // Big file open speed
    p = dir.filePath("big.txt");
    {
        QFile f(p); f.open(QIODevice::WriteOnly);
        QByteArray chunk;
        for (int i = 0; i < 100000; ++i) chunk += "Lorem ipsum dolor sit amet, consectetur adipiscing elit.\n";
        for (int i = 0; i < 180; ++i) f.write(chunk);  // ~1 GB
    }
    QElapsedTimer t; t.start();
    CHECK(e.openFile(p, err));
    e.repaint();
    fprintf(stderr, "open+paint 1GB: %lld ms\n", (long long)t.elapsed());
    t.restart();
    QTest::keyClick(&e, Qt::Key_End, Qt::ControlModifier);
    e.repaint();
    fprintf(stderr, "ctrl+end: %lld ms\n", (long long)t.elapsed());
    t.restart();
    {
        Searcher se(e.document());
        std::string er;
        se.setPattern("nonexistent", true, false, er);
        bool w;
        CHECK(!e.find(se, false, w));
        fprintf(stderr, "literal search 1GB: %lld ms\n", (long long)t.elapsed());
        t.restart();
        se.setPattern("NONEXISTENT", false, false, er);
        CHECK(!e.find(se, false, w));
        fprintf(stderr, "caseless search 1GB: %lld ms\n", (long long)t.elapsed());
        t.restart();
        se.setPattern("DoLoR", false, false, er);
        uint64_t cnt = 0;
        se.forEachMatch("", [&](uint64_t, uint64_t, std::string_view) { ++cnt; });
        CHECK(cnt == 18000000);
        fprintf(stderr, "caseless count 1GB: %lld ms\n", (long long)t.elapsed());
        t.restart();
        // event loop keeps running during long operations, and Esc-style cancel leaves the document unchanged
        {
            se.setPattern("dolor", true, false, er);
            int ticks = 0;
            QTimer tick;
            QObject::connect(&tick, &QTimer::timeout, [&] { ++ticks; });
            tick.start(20);
            qint64 cancelAt = 0; QElapsedTimer ct0; ct0.start(); QTimer::singleShot(400, [&] { cancelAt = ct0.elapsed(); e.cancelBusy(); });
            uint64_t before = e.document().size();
            QElapsedTimer ct; ct.start();
            CHECK(e.replaceAll(se, "DOLOR") == 0);
            CHECK(e.wasCancelled());
            CHECK(e.document().size() == before && e.document().text(0, 18) == "Lorem ipsum dolor ");
            CHECK(!e.isBusy());
            fprintf(stderr, "cancel requested at %lld ms, replaceAll returned after %lld ms, event loop ticks: %d\n", (long long)cancelAt, (long long)ct.elapsed(), ticks);
            CHECK(ticks >= 5);
            t.restart();
        }
        se.setPattern("dolor", true, false, er);
        CHECK(e.replaceAll(se, "DOLOR") == 18000000);
        fprintf(stderr, "replace all 18M: %lld ms, pieces=%llu\n", (long long)t.elapsed(),
                (unsigned long long)e.document().pieceCount());
        t.restart();
        typ(&e, "typing after replace");
        fprintf(stderr, "typing: %lld ms\n", (long long)t.elapsed());
        CHECK(e.document().text(0, 30) == "Lorem ipsum DOLOR sit amet, co");
    }
    fprintf(stderr, fails ? "%d FAILURES\n" : "ALL OK\n", fails);
    printf("%s\n", qPrintable(dir.path()));
    dir.setAutoRemove(false);
    return fails != 0;
}
