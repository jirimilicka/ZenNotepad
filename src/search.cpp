#define PCRE2_CODE_UNIT_WIDTH 8
#include "search.h"

#include <cstring>
#include <pcre2.h>

namespace {
const uint64_t kChunk = 4u << 20;   // bytes per search chunk
const uint64_t kSnap = 64u << 10;   // how far to look for a newline to align chunks
}

Searcher::~Searcher()
{
    if (md_)
        pcre2_match_data_free(md_);
    if (code_)
        pcre2_code_free(code_);
}

bool Searcher::setPattern(const std::string &pat, bool matchCase, bool regex, std::string &err)
{
    if (md_)
        pcre2_match_data_free(md_);
    if (code_)
        pcre2_code_free(code_);
    md_ = nullptr;
    code_ = nullptr;
    ok_ = false;
    pat_ = pat;
    regex_ = regex;
    literal_ = !regex && matchCase;
    if (pat.empty())
        return false;
    if (literal_) {
        ok_ = true;
        return true;
    }
    uint32_t opts = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF;
    opts |= regex ? (PCRE2_MULTILINE | PCRE2_UCP) : PCRE2_LITERAL;
    if (!matchCase)
        opts |= PCRE2_CASELESS;
    pcre2_compile_context *cc = pcre2_compile_context_create(nullptr);
    pcre2_set_newline(cc, PCRE2_NEWLINE_ANYCRLF);
    int ec;
    PCRE2_SIZE eo;
    code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pat.data()), pat.size(), opts, &ec, &eo, cc);
    pcre2_compile_context_free(cc);
    if (!code_) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(ec, buf, sizeof buf);
        err = reinterpret_cast<char *>(buf);
        return false;
    }
    pcre2_jit_compile(code_, PCRE2_JIT_COMPLETE);
    md_ = pcre2_match_data_create_from_pattern(code_, nullptr);
    ok_ = true;
    return true;
}

static uint64_t overlapFor(bool literal, size_t patLen)
{
    return literal ? patLen : std::max<uint64_t>(4096, patLen * 4);
}

void Searcher::chunkForward(uint64_t lo, uint64_t &a, uint64_t &e, uint64_t &hi, bool &notbol, bool &noteol)
{
    const uint64_t size = d_.size();
    const uint64_t ov = overlapFor(literal_, pat_.size());
    int64_t nl = d_.findBackward('\n', lo, lo > kSnap ? lo - kSnap : 0);
    if (nl >= 0)
        a = nl + 1;
    else if (lo <= kSnap)
        a = 0;
    else
        a = lo - ov;
    e = lo + kChunk;
    if (e >= size) {
        e = size;
        hi = size + 1;
    } else {
        int64_t n2 = d_.findForward('\n', e, e + kSnap);
        if (n2 >= 0) {
            e = n2 + 1;
            hi = e;
        } else {
            hi = e - ov;
        }
    }
    notbol = a > 0 && d_.byteAt(a - 1) != '\n';
    noteol = e < size && d_.byteAt(e - 1) != '\n';
}

// Iterates matches in chunk [a,e) whose start lies in [lo,hi). cb returns false to stop.
template <class F>
static void scanImpl(const std::string_view sv, bool literal, const std::string &pat, pcre2_code *code,
                     pcre2_match_data *md, uint64_t a, uint64_t lo, uint64_t hi, bool notbol, bool noteol, bool overlap, F cb)
{
    const char *data = sv.data();
    const size_t len = sv.size();
    size_t off = lo - a;
    if (literal) {
        while (off <= len) {
            const void *p = memmem(data + off, len - off, pat.data(), pat.size());
            if (!p)
                return;
            size_t s = static_cast<const char *>(p) - data;
            if (a + s >= hi)
                return;
            if (!cb(s, s + pat.size()))
                return;
            off = overlap ? s + 1 : s + pat.size();
        }
        return;
    }
    uint32_t opts = (notbol ? PCRE2_NOTBOL : 0) | (noteol ? PCRE2_NOTEOL : 0);
    while (off <= len) {
        int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(data), len, off, opts, md, nullptr);
        if (rc < 0)
            return;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        size_t s = ov[0], en = ov[1];
        if (a + s >= hi)
            return;
        if (!cb(s, en))
            return;
        if (en > s && !overlap) {
            off = en;
        } else {
            off = s + 1;
            while (off < len && (static_cast<unsigned char>(data[off]) & 0xC0) == 0x80)
                ++off;
        }
    }
}

bool Searcher::scan(uint64_t a, uint64_t e, uint64_t lo, uint64_t hi, bool wantLast, uint64_t &ms, uint64_t &me)
{
    const uint64_t size = d_.size();
    bool notbol = a > 0 && d_.byteAt(a - 1) != '\n';
    bool noteol = e < size && d_.byteAt(e - 1) != '\n';
    std::string_view sv = d_.view(a, e, scratch_);
    if (a == e)
        sv = std::string_view("", 0);
    bool found = false;
    scanImpl(sv, literal_, pat_, code_, md_, a, lo, hi, notbol, noteol, wantLast, [&](size_t s, size_t en) {
        ms = a + s;
        me = a + en;
        found = true;
        return wantLast;
    });
    return found;
}

bool Searcher::findForward(uint64_t from, uint64_t &ms, uint64_t &me)
{
    if (!ok_)
        return false;
    const uint64_t size = d_.size();
    uint64_t lo = from;
    while (lo <= size) {
        uint64_t a, e, hi;
        bool nb, ne;
        chunkForward(lo, a, e, hi, nb, ne);
        if (scan(a, e, lo, hi, false, ms, me))
            return true;
        if (hi > size)
            break;
        lo = hi;
    }
    return false;
}

bool Searcher::findBackward(uint64_t before, uint64_t &ms, uint64_t &me)
{
    if (!ok_)
        return false;
    const uint64_t size = d_.size();
    const uint64_t ov = overlapFor(literal_, pat_.size());
    if (before > size + 1)
        before = size + 1;
    uint64_t hi = before;
    while (hi > 0) {
        uint64_t a0 = hi > kChunk ? hi - kChunk : 0;
        uint64_t a, lo;
        if (a0 == 0) {
            a = lo = 0;
        } else {
            int64_t nl = d_.findBackward('\n', a0, a0 > kSnap ? a0 - kSnap : 0);
            if (nl >= 0) {
                a = lo = nl + 1;
            } else {
                lo = a0;
                a = a0 > ov ? a0 - ov : 0;
            }
        }
        uint64_t e;
        uint64_t h = std::min(hi, size);
        int64_t n2 = d_.findForward('\n', h, h + kSnap);
        e = n2 >= 0 ? n2 + 1 : std::min(size, h + ov);
        if (scan(a, e, lo, hi, true, ms, me))
            return true;
        hi = lo;
    }
    return false;
}

bool Searcher::expand(std::string_view subj, const std::string &repl, std::string &out)
{
    if (!regex_) {
        out = repl;
        return true;
    }
    PCRE2_SIZE outlen = repl.size() * 2 + 64;
    out.resize(outlen);
    uint32_t opts = PCRE2_SUBSTITUTE_MATCHED | PCRE2_SUBSTITUTE_REPLACEMENT_ONLY | PCRE2_SUBSTITUTE_EXTENDED |
                    PCRE2_SUBSTITUTE_OVERFLOW_LENGTH | PCRE2_SUBSTITUTE_UNSET_EMPTY | PCRE2_SUBSTITUTE_UNKNOWN_UNSET;
    for (int attempt = 0; attempt < 2; ++attempt) {
        int rc = pcre2_substitute(code_, reinterpret_cast<PCRE2_SPTR>(subj.data()), subj.size(), 0, opts, md_,
                                  nullptr, reinterpret_cast<PCRE2_SPTR>(repl.data()), repl.size(),
                                  reinterpret_cast<PCRE2_UCHAR *>(out.data()), &outlen);
        if (rc >= 0) {
            out.resize(outlen);
            return true;
        }
        if (rc != PCRE2_ERROR_NOMEMORY)
            break;
        out.resize(outlen);
    }
    out = repl;
    return false;
}

bool Searcher::matchesExactly(uint64_t a, uint64_t b, const std::string &repl, std::string &out)
{
    if (!ok_)
        return false;
    if (literal_) {
        if (d_.text(a, b) != pat_)
            return false;
        out = repl;
        return true;
    }
    const uint64_t size = d_.size();
    int64_t nl = d_.findBackward('\n', a, a > kSnap ? a - kSnap : 0);
    uint64_t s = nl >= 0 ? nl + 1 : (a > kSnap ? a - 4096 : 0);
    int64_t n2 = d_.findForward('\n', b, b + kSnap);
    uint64_t e = n2 >= 0 ? n2 + 1 : std::min(size, b + 4096);
    std::string sc;
    std::string_view sv = d_.view(s, e, sc);
    if (s == e)
        sv = std::string_view("", 0);
    uint32_t opts = PCRE2_ANCHORED;
    if (s > 0 && d_.byteAt(s - 1) != '\n')
        opts |= PCRE2_NOTBOL;
    if (e < size && d_.byteAt(e - 1) != '\n')
        opts |= PCRE2_NOTEOL;
    int rc = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(sv.data()), sv.size(), a - s, opts, md_, nullptr);
    if (rc < 0)
        return false;
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md_);
    if (ov[0] != a - s || ov[1] != b - s)
        return false;
    expand(sv, repl, out);
    return true;
}

void Searcher::forEachMatch(const std::string &repl,
                            const std::function<void(uint64_t, uint64_t, std::string_view)> &cb)
{
    if (!ok_)
        return;
    const uint64_t size = d_.size();
    uint64_t lo = 0;
    std::string r;
    while (lo <= size) {
        uint64_t a, e, hi;
        bool nb, ne;
        chunkForward(lo, a, e, hi, nb, ne);
        std::string_view sv = d_.view(a, e, scratch_);
        if (a == e)
            sv = std::string_view("", 0);
        uint64_t lastEnd = lo;
        scanImpl(sv, literal_, pat_, code_, md_, a, lo, hi, nb, ne, false, [&](size_t s, size_t en) {
            if (literal_)
                cb(a + s, a + en, repl);
            else {
                expand(sv, repl, r);
                cb(a + s, a + en, r);
            }
            lastEnd = a + en;
            return true;
        });
        if (hi > size)
            break;
        lo = std::max(hi, lastEnd);
    }
}
