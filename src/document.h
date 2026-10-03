// Document: an immutable original buffer (mmap'd file) + append-only add buffer,
// indexed by an implicit treap of pieces. All edits go through replace().
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Piece {
    uint64_t off;
    uint64_t len;
    uint32_t buf;
};

class Document {
public:
    Document();
    ~Document();
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    // Opens a file (mmap). Returns false and fills err on failure.
    bool open(const std::string &path, std::string &err);
    void clear();
    // Saves atomically (temp file + rename). If that is impossible it writes in place;
    // then historyInvalidated is set and old undo pieces must be discarded.
    bool save(const std::string &path, std::string &err, bool &historyInvalidated);

    enum class SaveResult { Ok, Failed, Cancelled, NeedsInPlace };
    // The atomic part of save(). Does not modify the document, so it may run in a worker
    // thread. progress (bytes written) and cancel may be null.
    SaveResult saveAtomic(const std::string &path, std::string &err, std::atomic<uint64_t> *progress,
                          const std::atomic<bool> *cancel) const;
    // Fallback when the directory is not writable. Modifies the document; main thread only.
    bool saveInPlace(const std::string &path, std::string &err, bool &historyInvalidated);

    uint64_t size() const;

    // Pointer to contiguous bytes starting at pos; avail = bytes until piece end.
    const char *chunkAt(uint64_t pos, uint64_t &avail) const;
    unsigned char byteAt(uint64_t pos) const;
    std::string text(uint64_t a, uint64_t b) const;
    // View of [a,b); copies into scratch only if the range spans several pieces.
    std::string_view view(uint64_t a, uint64_t b, std::string &scratch) const;

    // Position of first byte c in [from, to), or -1.
    int64_t findForward(char c, uint64_t from, uint64_t to) const;
    // Position of last byte c in [to, from), or -1.
    int64_t findBackward(char c, uint64_t from, uint64_t to) const;

    // Append bytes to the add buffer, returning the piece describing them.
    Piece addText(std::string_view s);
    // Take ownership of a whole buffer (no copy).
    Piece addOwned(std::string &&s);

    // Core primitive: replace [pos, pos+len) with the given pieces. Returns removed pieces.
    std::vector<Piece> replace(uint64_t pos, uint64_t len, const std::vector<Piece> &pieces);
    std::vector<Piece> allPieces() const;
    static uint64_t piecesLength(const std::vector<Piece> &p);
    uint64_t pieceCount() const { return nodeCount_; }

    const char *bufData(uint32_t b) const;

private:
    struct Node {
        uint64_t off, len, sum;
        uint32_t l, r, prio, buf;
    };
    uint32_t newNode(const Piece &p);
    void freeTree(uint32_t t);
    void upd(uint32_t t) { auto &n = nodes_[t]; n.sum = nodes_[n.l].sum + n.len + nodes_[n.r].sum; }
    void split(uint32_t t, uint64_t pos, uint32_t &a, uint32_t &b);
    uint32_t merge(uint32_t a, uint32_t b);
    uint32_t build(const std::vector<Piece> &p);
    void collect(uint32_t t, std::vector<Piece> &out) const;
    bool extendLast(uint32_t t, const Piece &p);
    uint32_t rnd();

    std::vector<Node> nodes_;
    std::vector<uint32_t> free_;
    uint32_t root_ = 0;
    uint64_t nodeCount_ = 0;
    uint32_t seed_ = 2463534242u;

    // buffer 0: mmap'd original, buffer i>0: add chunk i-1. Chunks never reallocate,
    // so pointers into them stay valid.
    const char *map_ = nullptr;
    uint64_t mapLen_ = 0;
    std::vector<std::unique_ptr<std::string>> chunks_;
};
