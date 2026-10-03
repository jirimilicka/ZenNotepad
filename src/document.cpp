#include "document.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

Document::Document()
{
    nodes_.push_back(Node{0, 0, 0, 0, 0, 0, 0}); // index 0 = nil
}

Document::~Document()
{
    if (map_)
        munmap(const_cast<char *>(map_), mapLen_);
}

void Document::clear()
{
    if (map_)
        munmap(const_cast<char *>(map_), mapLen_);
    map_ = nullptr;
    mapLen_ = 0;
    chunks_.clear();
    nodes_.resize(1);
    nodes_.shrink_to_fit();
    free_.clear();
    root_ = 0;
    nodeCount_ = 0;
}

bool Document::open(const std::string &path, std::string &err)
{
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = strerror(errno);
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        err = "Not a regular file";
        ::close(fd);
        return false;
    }
    void *m = nullptr;
    if (st.st_size > 0) {
        m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
        if (m == MAP_FAILED) {
            err = strerror(errno);
            ::close(fd);
            return false;
        }
    }
    ::close(fd);
    clear();
    map_ = static_cast<const char *>(m);
    mapLen_ = st.st_size;
    if (mapLen_)
        root_ = newNode(Piece{0, mapLen_, 0});
    return true;
}

static bool writeAll(int fd, const char *p, uint64_t n)
{
    while (n) {
        ssize_t w = ::write(fd, p, n > (1u << 30) ? (1u << 30) : n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += w;
        n -= w;
    }
    return true;
}

bool Document::save(const std::string &path, std::string &err, bool &historyInvalidated)
{
    historyInvalidated = false;
    switch (saveAtomic(path, err, nullptr, nullptr)) {
    case SaveResult::Ok: return true;
    case SaveResult::NeedsInPlace: return saveInPlace(path, err, historyInvalidated);
    default: return false;
    }
}

Document::SaveResult Document::saveAtomic(const std::string &path, std::string &err,
                                          std::atomic<uint64_t> *progress, const std::atomic<bool> *cancel) const
{
    std::vector<Piece> ps = allPieces();

    struct stat st;
    bool exists = ::stat(path.c_str(), &st) == 0;
    mode_t mode = exists ? (st.st_mode & 07777) : 0666;

    std::string tmp = path + ".zen-notepad-XXXXXX";
    std::vector<char> tbuf(tmp.begin(), tmp.end());
    tbuf.push_back(0);
    int fd = mkstemp(tbuf.data());
    if (fd < 0)
        return SaveResult::NeedsInPlace;
    bool ok = true, cancelled = false;
    uint64_t written = 0;
    const uint64_t kStep = 64u << 20;  // write in steps to report progress and allow cancelling
    for (const Piece &p : ps) {
        for (uint64_t o = 0; ok && o < p.len; o += kStep) {
            if (cancel && cancel->load()) {
                cancelled = true;
                break;
            }
            uint64_t n = std::min(kStep, p.len - o);
            ok = writeAll(fd, bufData(p.buf) + p.off + o, n);
            written += n;
            if (progress)
                progress->store(written);
        }
        if (!ok || cancelled)
            break;
    }
    if (ok && !cancelled) {
        if (exists) {
            fchmod(fd, mode);
        } else {
            mode_t um = umask(0);
            umask(um);
            fchmod(fd, 0666 & ~um);
        }
        ok = fdatasync(fd) == 0;
    }
    int e = ok ? 0 : errno;
    ::close(fd);
    if (ok && !cancelled) {
        if (::rename(tbuf.data(), path.c_str()) == 0)
            return SaveResult::Ok;
        e = errno;
    }
    ::unlink(tbuf.data());
    if (cancelled)
        return SaveResult::Cancelled;
    err = strerror(e);
    return SaveResult::Failed;
}

bool Document::saveInPlace(const std::string &path, std::string &err, bool &historyInvalidated)
{
    historyInvalidated = false;
    std::vector<Piece> ps = allPieces();
    // The mapped original may be this very file, so first move all content into memory.
    bool usesMap = false;
    for (const Piece &p : ps)
        usesMap |= p.buf == 0;
    if (usesMap) {
        std::string all;
        all.reserve(size());
        for (const Piece &p : ps)
            all.append(bufData(p.buf) + p.off, p.len);
        nodes_.resize(1);
        free_.clear();
        nodeCount_ = 0;
        root_ = 0;
        if (!all.empty()) {
            Piece pc = addOwned(std::move(all));
            root_ = newNode(pc);
        }
        if (map_)
            munmap(const_cast<char *>(map_), mapLen_);
        map_ = nullptr;
        mapLen_ = 0;
        historyInvalidated = true;
        ps = allPieces();
    }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        err = strerror(errno);
        return false;
    }
    bool ok = true;
    for (const Piece &p : ps)
        if (!(ok = writeAll(fd, bufData(p.buf) + p.off, p.len)))
            break;
    if (!ok)
        err = strerror(errno);
    ::close(fd);
    return ok;
}

const char *Document::bufData(uint32_t b) const
{
    return b == 0 ? map_ : chunks_[b - 1]->data();
}

uint64_t Document::size() const
{
    return nodes_[root_].sum;
}

uint32_t Document::rnd()
{
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_;
}

uint32_t Document::newNode(const Piece &p)
{
    uint32_t i;
    if (!free_.empty()) {
        i = free_.back();
        free_.pop_back();
    } else {
        i = nodes_.size();
        nodes_.push_back({});
    }
    nodes_[i] = Node{p.off, p.len, p.len, 0, 0, rnd(), p.buf};
    ++nodeCount_;
    return i;
}

void Document::freeTree(uint32_t t)
{
    if (!t)
        return;
    std::vector<uint32_t> st{t};
    while (!st.empty()) {
        uint32_t n = st.back();
        st.pop_back();
        if (nodes_[n].l)
            st.push_back(nodes_[n].l);
        if (nodes_[n].r)
            st.push_back(nodes_[n].r);
        free_.push_back(n);
        --nodeCount_;
    }
}

void Document::split(uint32_t t, uint64_t pos, uint32_t &a, uint32_t &b)
{
    if (!t) {
        a = b = 0;
        return;
    }
    uint64_t ls = nodes_[nodes_[t].l].sum;
    uint64_t len = nodes_[t].len;
    if (pos <= ls) {
        uint32_t x, y;
        split(nodes_[t].l, pos, x, y);
        nodes_[t].l = y;
        upd(t);
        a = x;
        b = t;
    } else if (pos >= ls + len) {
        uint32_t x, y;
        split(nodes_[t].r, pos - ls - len, x, y);
        nodes_[t].r = x;
        upd(t);
        a = t;
        b = y;
    } else {
        uint64_t k = pos - ls;
        uint32_t n2 = newNode(Piece{nodes_[t].off + k, len - k, nodes_[t].buf});
        nodes_[n2].prio = nodes_[t].prio;
        nodes_[n2].r = nodes_[t].r;
        upd(n2);
        nodes_[t].len = k;
        nodes_[t].r = 0;
        upd(t);
        a = t;
        b = n2;
    }
}

uint32_t Document::merge(uint32_t a, uint32_t b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    if (nodes_[a].prio >= nodes_[b].prio) {
        nodes_[a].r = merge(nodes_[a].r, b);
        upd(a);
        return a;
    }
    nodes_[b].l = merge(a, nodes_[b].l);
    upd(b);
    return b;
}

uint32_t Document::build(const std::vector<Piece> &ps)
{
    // Cartesian tree construction in O(n).
    std::vector<uint32_t> st;
    for (const Piece &p : ps) {
        if (!p.len)
            continue;
        uint32_t n = newNode(p);
        uint32_t last = 0;
        while (!st.empty() && nodes_[st.back()].prio < nodes_[n].prio) {
            last = st.back();
            st.pop_back();
        }
        nodes_[n].l = last;
        if (!st.empty())
            nodes_[st.back()].r = n;
        st.push_back(n);
    }
    if (st.empty())
        return 0;
    // fix sums bottom-up: post-order traversal
    uint32_t root = st.front();
    std::vector<std::pair<uint32_t, bool>> s{{root, false}};
    while (!s.empty()) {
        auto [n, done] = s.back();
        s.pop_back();
        if (done) {
            upd(n);
            continue;
        }
        s.push_back({n, true});
        if (nodes_[n].l)
            s.push_back({nodes_[n].l, false});
        if (nodes_[n].r)
            s.push_back({nodes_[n].r, false});
    }
    return root;
}

void Document::collect(uint32_t t, std::vector<Piece> &out) const
{
    std::vector<uint32_t> st;
    while (t || !st.empty()) {
        while (t) {
            st.push_back(t);
            t = nodes_[t].l;
        }
        t = st.back();
        st.pop_back();
        out.push_back(Piece{nodes_[t].off, nodes_[t].len, nodes_[t].buf});
        t = nodes_[t].r;
    }
}

bool Document::extendLast(uint32_t t, const Piece &p)
{
    if (!t)
        return false;
    if (nodes_[t].r) {
        if (!extendLast(nodes_[t].r, p))
            return false;
        upd(t);
        return true;
    }
    Node &n = nodes_[t];
    if (n.buf == p.buf && n.off + n.len == p.off) {
        n.len += p.len;
        upd(t);
        return true;
    }
    return false;
}

std::vector<Piece> Document::replace(uint64_t pos, uint64_t len, const std::vector<Piece> &pieces)
{
    uint32_t a, r, m, b;
    split(root_, pos, a, r);
    split(r, len, m, b);
    std::vector<Piece> old;
    collect(m, old);
    freeTree(m);
    uint32_t mid = 0;
    if (pieces.size() == 1 && extendLast(a, pieces[0])) {
        mid = 0;
    } else {
        mid = build(pieces);
    }
    root_ = merge(merge(a, mid), b);
    return old;
}

std::vector<Piece> Document::allPieces() const
{
    std::vector<Piece> v;
    v.reserve(nodeCount_);
    collect(root_, v);
    return v;
}

uint64_t Document::piecesLength(const std::vector<Piece> &p)
{
    uint64_t s = 0;
    for (auto &x : p)
        s += x.len;
    return s;
}

Piece Document::addText(std::string_view s)
{
    const size_t kChunk = 4u << 20;
    if (s.size() >= kChunk / 4)
        return addOwned(std::string(s));
    if (chunks_.empty() || chunks_.back()->capacity() - chunks_.back()->size() < s.size()) {
        chunks_.push_back(std::make_unique<std::string>());
        chunks_.back()->reserve(kChunk);
    }
    std::string &c = *chunks_.back();
    Piece p{c.size(), s.size(), uint32_t(chunks_.size())};
    c.append(s);
    return p;
}

Piece Document::addOwned(std::string &&s)
{
    // Inserted before the open small chunk would renumber it, so it simply goes last;
    // the next small insert then opens a fresh chunk because this one is full.
    s.shrink_to_fit();
    Piece p{0, s.size(), 0};
    chunks_.push_back(std::make_unique<std::string>(std::move(s)));
    chunks_.back()->shrink_to_fit();
    p.buf = uint32_t(chunks_.size());
    return p;
}

const char *Document::chunkAt(uint64_t pos, uint64_t &avail) const
{
    uint32_t t = root_;
    while (t) {
        const Node &n = nodes_[t];
        uint64_t ls = nodes_[n.l].sum;
        if (pos < ls) {
            t = n.l;
        } else if (pos < ls + n.len) {
            uint64_t k = pos - ls;
            avail = n.len - k;
            return bufData(n.buf) + n.off + k;
        } else {
            pos -= ls + n.len;
            t = n.r;
        }
    }
    avail = 0;
    return nullptr;
}

unsigned char Document::byteAt(uint64_t pos) const
{
    uint64_t av;
    const char *p = chunkAt(pos, av);
    return p ? (unsigned char)*p : 0;
}

std::string Document::text(uint64_t a, uint64_t b) const
{
    std::string s;
    if (b > size())
        b = size();
    if (a >= b)
        return s;
    s.reserve(b - a);
    while (a < b) {
        uint64_t av;
        const char *p = chunkAt(a, av);
        if (!p)
            break;
        uint64_t n = std::min(av, b - a);
        s.append(p, n);
        a += n;
    }
    return s;
}

std::string_view Document::view(uint64_t a, uint64_t b, std::string &scratch) const
{
    if (b > size())
        b = size();
    if (a >= b)
        return {};
    uint64_t av;
    const char *p = chunkAt(a, av);
    if (av >= b - a)
        return std::string_view(p, b - a);
    scratch = text(a, b);
    return scratch;
}

int64_t Document::findForward(char c, uint64_t from, uint64_t to) const
{
    if (to > size())
        to = size();
    while (from < to) {
        uint64_t av;
        const char *p = chunkAt(from, av);
        if (!p)
            return -1;
        uint64_t n = std::min(av, to - from);
        const void *q = memchr(p, c, n);
        if (q)
            return from + (static_cast<const char *>(q) - p);
        from += n;
    }
    return -1;
}

int64_t Document::findBackward(char c, uint64_t from, uint64_t to) const
{
    if (from > size())
        from = size();
    while (from > to) {
        uint64_t av;
        const char *p = chunkAt(from - 1, av);
        if (!p)
            return -1;
        // find piece start: chunkAt gives pointer to byte from-1; walk back within piece
        // by asking for the piece containing it. We need the piece start offset.
        uint64_t pieceStartPos = from - 1;
        {
            uint32_t t = root_;
            uint64_t pos = from - 1, base = 0;
            while (t) {
                const Node &n = nodes_[t];
                uint64_t ls = nodes_[n.l].sum;
                if (pos < ls) {
                    t = n.l;
                } else if (pos < ls + n.len) {
                    pieceStartPos = base + ls;
                    break;
                } else {
                    pos -= ls + n.len;
                    base += ls + n.len;
                    t = n.r;
                }
            }
        }
        uint64_t lo = std::max(pieceStartPos, to);
        uint64_t n = from - lo;
        const char *start = p - (from - 1 - lo);
        const void *q = memrchr(start, c, n);
        if (q)
            return lo + (static_cast<const char *>(q) - start);
        from = lo;
    }
    return -1;
}
