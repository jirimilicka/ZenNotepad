#pragma once
#include "document.h"

#include <functional>
#include <string>

typedef struct pcre2_real_code_8 pcre2_code_8;
typedef struct pcre2_real_match_data_8 pcre2_match_data_8;

// Searches a Document in newline-aligned chunks. Plain case-sensitive text uses memmem,
// everything else (case-insensitive, regex) uses PCRE2 with JIT directly on UTF-8.
class Searcher {
public:
    explicit Searcher(const Document &d) : d_(d) {}
    ~Searcher();

    bool setPattern(const std::string &pat, bool matchCase, bool regex, std::string &err);
    bool valid() const { return ok_; }

    // First match starting at >= from. Empty matches allowed for regex.
    bool findForward(uint64_t from, uint64_t &ms, uint64_t &me);
    // Last match starting at < before.
    bool findBackward(uint64_t before, uint64_t &ms, uint64_t &me);
    // Is [a,b) exactly a match? Fills the expanded replacement if so.
    bool matchesExactly(uint64_t a, uint64_t b, const std::string &repl, std::string &out);

    // Calls cb(ms, me, replacement) for every non-overlapping match in document order.
    // Returns false if cb returns false (abort).
    void forEachMatch(const std::string &repl,
                      const std::function<void(uint64_t, uint64_t, std::string_view)> &cb);

private:
    // Scan chunk [a,e) for matches with start in [lo,hi). If wantLast, return the last one.
    bool scan(uint64_t a, uint64_t e, uint64_t lo, uint64_t hi, bool wantLast, uint64_t &ms, uint64_t &me);
    void chunkForward(uint64_t lo, uint64_t &a, uint64_t &e, uint64_t &hi, bool &notbol, bool &noteol);
    bool expand(std::string_view subj, const std::string &repl, std::string &out);

    const Document &d_;
    std::string pat_;
    bool literal_ = true;
    bool regex_ = false;
    bool ok_ = false;
    pcre2_code_8 *code_ = nullptr;
    pcre2_match_data_8 *md_ = nullptr;
    std::string scratch_;
};
