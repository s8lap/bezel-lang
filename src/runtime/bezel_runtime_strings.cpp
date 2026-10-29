// bezel_runtime_strings.cpp — string search/slice, StringBuilder mutators, CString/WString/RawBytes.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// §4.6 / #535: Width-aware (code-point-level) string search/slice helpers.
//
// The internal bz.string representation stores code points at 1/2/4 bytes each
// depending on the maximum code point value in the string (§4.6 adaptive-width
// storage). Byte-level search primitives (memmem, memcmp) operating on the raw
// buffer are WRONG for width>1: passing `len` (code-point count) as a byte
// size reads only the first `len` out of `len*width` actual data bytes.
//
// These helpers accept full bz_string* pointers, read each operand's width
// field, and compare code points by value regardless of storage width. All
// index/length arguments and return values are CODE-POINT counts/indices, not
// byte offsets, matching §9.6's documented semantics.
//
// Helper: read one code point from a string's data buffer at code-point index i.
static inline uint32_t bz_str_cp(const bz_string_struct* s, int64_t i) {
    if (s->width == 1) return ((const uint8_t*)s->data)[i];
    if (s->width == 2) return ((const uint16_t*)s->data)[i];
    return ((const uint32_t*)s->data)[i];
}

// stdlib §9.6: IsAscii — "true iff every code point < 128".
//
// #7245: the predicate is defined by the CONTENT, never by the storage width.
// §4.6 (v0.344) makes the width an invisible implementation detail ("The choice
// of width is invisible at the language level; programs observe only the `char`
// sequence and its length"), and it is only *typically* the narrowest that
// fits the value — a legal string may carry a wider width than its content
// needs (e.g. the range slice of a wide string, whose result keeps the source
// width). The old shortcut `if (s->width != 1) return 0;` therefore reported
// FALSE for an all-ASCII string that merely happened to be stored two bytes
// per code point. Answer from the code points instead: scan in the code-point
// domain so that every legal width gives the same, content-determined answer.
//
// The stdlib's "(O(1))" note presumes the width is always the narrowest that
// fits (§4.6), which would make "width ≥ 2 ⇒ some code point ≥ 256 ⇒ not
// ASCII" a constant-time decision. That premise does not hold for every legal
// width, so the answer is computed from the content; a width-1 string still
// needs the scan for a byte in [128,255].
int32_t bz_string_is_ascii(const void* sp) {
    const bz_string_struct* s = (const bz_string_struct*)sp;
    if (!s || s->len == 0) return 1;            // the empty string is ASCII
    // Every string the runtime builds stores 1, 2 or 4 bytes per code point
    // (§4.6). A width outside that set is malformed; answer conservatively
    // rather than reading the buffer with a width it does not have.
    if (s->width != 1 && s->width != 2 && s->width != 4) return 0;
    for (int64_t i = 0; i < s->len; ++i)
        if (bz_str_cp(s, i) >= 128) return 0;    // a code point ≥ 128 defeats ASCII
    return 1;
}

// Helper: build a fresh +1 bz_string from a sub-range [start, start+count) of
// source string `s`. The result uses the NARROWEST width that fits all its code
// points (§4.6 adaptive-width storage). This allows comparing a substring of
// an ASCII segment extracted from a wide string against a width=1 literal using
// emitStringEquals's memcmp path (which only works for equal-width operands).
static void* bz_str_slice_cp(const bz_string_struct* s, int64_t start, int64_t count) {
    if (count < 0) count = 0;
    // Scan to find the maximum code point value in the range.
    uint32_t maxCp = 0;
    for (int64_t i = 0; i < count; ++i) {
        uint32_t cp = bz_str_cp(s, start + i);
        if (cp > maxCp) maxCp = cp;
    }
    // Select narrowest width (§4.6 adaptive storage).
    int8_t w = (maxCp <= 0xFF) ? 1 : (maxCp <= 0xFFFF ? 2 : 4);
    size_t byte_count = (size_t)count * (size_t)w;
    // Allocate data buffer (with one width-sized terminator slot).
    void* data = calloc(byte_count + (size_t)w, 1);
    if (w == 1) {
        uint8_t* d = (uint8_t*)data;
        for (int64_t i = 0; i < count; ++i) d[i] = (uint8_t)bz_str_cp(s, start + i);
    } else if (w == 2) {
        uint16_t* d = (uint16_t*)data;
        for (int64_t i = 0; i < count; ++i) d[i] = (uint16_t)bz_str_cp(s, start + i);
    } else {
        uint32_t* d = (uint32_t*)data;
        for (int64_t i = 0; i < count; ++i) d[i] = bz_str_cp(s, start + i);
    }
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &count, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// §9.6 / #1295: PadLeft/PadRight/PadCenter — build a fresh +1 bz_string of
// `totalWidth` code points by padding `self` with `padChar` (a code point).
// `align` selects the layout:
//   0 = PadLeft  (pad first, self after)
//   1 = PadRight (self at the front, pad after)
//   2 = PadCenter (the padding is split EVENLY; when the split is odd the
//       EXTRA character goes to the RIGHT — stdlib §9.6: the padding is
//       "split evenly; when it is odd, the extra character goes to the
//       RIGHT: \"ab\".PadCenter(5) = \" ab  \"", i.e. one pad char on the
//       left and two on the right for a 3-code-point padding).
// Strings store FIXED-WIDTH code points (width 1/2/4 bytes/cp,
// §4.6); the result width is the widest of self's width and the pad char's
// width, and self's code points are widened into it. The previous inline
// codegen assumed 1 byte per code point (memcpy'd the code-point COUNT as a
// byte count and hard-stored width=1), corrupting any code point > U+00FF.
extern "C" void* bz_str_pad(const void* self_p, int64_t totalWidth,
                            int32_t padChar, int8_t align) {
    const bz_string_struct* s = (const bz_string_struct*)self_p;
    int64_t selfLen = s ? s->len : 0;
    // No padding needed: return a fresh copy of the whole string (own +1),
    // matching the "return self unchanged" contract in a copy-returning form.
    if (totalWidth <= selfLen)
        return bz_str_slice_cp(s, 0, selfLen);
    int64_t padCount = totalWidth - selfLen;
    uint32_t pc = (uint32_t)padChar;
    int8_t padW  = (pc <= 0xFF) ? 1 : (pc <= 0xFFFF ? 2 : 4);
    int8_t selfW = s ? s->width : 1;
    int8_t w = padW > selfW ? padW : selfW;
    size_t byte_count = (size_t)totalWidth * (size_t)w;
    void* data = calloc(byte_count + (size_t)w, 1);  // + width-sized NUL slot
    #define BZ_PAD_PUT(idx, cp)                                       \
        do {                                                          \
            if (w == 1)      ((uint8_t*)data)[(idx)]  = (uint8_t)(cp);\
            else if (w == 2) ((uint16_t*)data)[(idx)] = (uint16_t)(cp);\
            else             ((uint32_t*)data)[(idx)] = (uint32_t)(cp);\
        } while (0)
    if (align == 2) {
        // PadCenter: the padding is split EVENLY; when the split is ODD the
        // extra code point goes to the RIGHT (stdlib §9.6: "ab".PadCenter(5)
        // = " ab  " — one pad char on the left, two on the right).
        int64_t leftPad = padCount / 2;          // floor → the remainder is right
        int64_t rightPad = padCount - leftPad;
        for (int64_t i = 0; i < leftPad; ++i)    BZ_PAD_PUT(i, pc);
        for (int64_t i = 0; i < selfLen; ++i)    BZ_PAD_PUT(leftPad + i, bz_str_cp(s, i));
        for (int64_t i = 0; i < rightPad; ++i)   BZ_PAD_PUT(leftPad + selfLen + i, pc);
    } else if (align == 1) {
        // PadRight: self at [0, selfLen), pad char fills [selfLen, totalWidth).
        for (int64_t i = 0; i < selfLen; ++i)   BZ_PAD_PUT(i, bz_str_cp(s, i));
        for (int64_t i = selfLen; i < totalWidth; ++i) BZ_PAD_PUT(i, pc);
    } else {
        // PadLeft: pad char fills [0, padCount), self at [padCount, totalWidth).
        for (int64_t i = 0; i < padCount; ++i)  BZ_PAD_PUT(i, pc);
        for (int64_t i = 0; i < selfLen; ++i)   BZ_PAD_PUT(padCount + i, bz_str_cp(s, i));
    }
    #undef BZ_PAD_PUT
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &totalWidth, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// Helper: compare two code-point sequences for equality.
// Returns 1 if they have the same length and the same code point at every
// position, 0 otherwise. Width of each may differ.
static int bz_str_cp_seq_eq(const bz_string_struct* a, int64_t a_start,
                             const bz_string_struct* b, int64_t b_start,
                             int64_t len) {
    for (int64_t i = 0; i < len; ++i) {
        if (bz_str_cp(a, a_start + i) != bz_str_cp(b, b_start + i)) return 0;
    }
    return 1;
}

// per §9.6 / #535: StartsWith(prefix: string): bool — code-point comparison.
// Returns 1 if every code point of `prefix` matches the corresponding leading
// code point of `self`, 0 otherwise. Width mismatch is handled via bz_str_cp.
extern "C" int32_t bz_str_startswith(const void* self_p, const void* prefix_p) {
    if (!self_p || !prefix_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* p = (const bz_string_struct*)prefix_p;
    if (p->len > s->len) return 0;
    return bz_str_cp_seq_eq(s, 0, p, 0, p->len);
}

// per §9.6 / #535: EndsWith(suffix: string): bool — code-point comparison.
extern "C" int32_t bz_str_endswith(const void* self_p, const void* suffix_p) {
    if (!self_p || !suffix_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* suf = (const bz_string_struct*)suffix_p;
    if (suf->len > s->len) return 0;
    int64_t offset = s->len - suf->len;
    return bz_str_cp_seq_eq(s, offset, suf, 0, suf->len);
}

// per §9.6 / #535: Contains(sub: string): bool — code-point sliding window.
// Returns 1 if `sub` appears as a consecutive code-point subsequence anywhere
// in `self`; 0 otherwise.
extern "C" int32_t bz_str_contains(const void* self_p, const void* sub_p) {
    if (!self_p || !sub_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)sub_p;
    if (n->len == 0) return 1;   // empty needle is always contained (§9.6)
    if (n->len > s->len) return 0;
    int64_t limit = s->len - n->len;
    for (int64_t i = 0; i <= limit; ++i) {
        if (bz_str_cp_seq_eq(s, i, n, 0, n->len)) return 1;
    }
    return 0;
}

// per §9.6 / #535: IndexOf(sub: string): int — first code-point occurrence.
// Returns the code-point index of the first occurrence of `sub` in `self`,
// or -1 if not found. An empty needle returns 0 (per §9.6 "appears at 0").
extern "C" int32_t bz_str_indexof(const void* self_p, const void* sub_p) {
    if (!self_p || !sub_p) return -1;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)sub_p;
    if (n->len == 0) return 0;
    if (n->len > s->len) return -1;
    int64_t limit = s->len - n->len;
    for (int64_t i = 0; i <= limit; ++i) {
        if (bz_str_cp_seq_eq(s, i, n, 0, n->len)) return (int32_t)i;
    }
    return -1;
}

// per §9.6 / #649: IndexOf(sub: string, start: int): int — first code-point
// occurrence at or after code-point index `start`.
// Returns the code-point index of the first occurrence of `sub` in `self`
// starting at code-point position `start`, or -1 if not found.
// A negative `start` is treated as 0; a `start` >= self.len returns -1 unless
// `sub` is empty (which returns `start` clamped to self.len per §9.6).
// The `start` parameter is a code-point index (§4.6 adaptive-width strings).
extern "C" int32_t bz_str_indexof_from(const void* self_p, const void* sub_p,
                                        int64_t start) {
    if (!self_p || !sub_p) return -1;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)sub_p;
    if (start < 0) start = 0;
    if (n->len == 0) {
        // empty needle: return `start` clamped to s->len
        return (int32_t)(start <= s->len ? start : s->len);
    }
    if (start > s->len - n->len) return -1;
    int64_t limit = s->len - n->len;
    for (int64_t i = start; i <= limit; ++i) {
        if (bz_str_cp_seq_eq(s, i, n, 0, n->len)) return (int32_t)i;
    }
    return -1;
}

// per §9.6 / #535: LastIndexOf(sub: string): int — last code-point occurrence.
// Returns the code-point index of the LAST occurrence of `sub` in `self`,
// or -1 if not found.
extern "C" int32_t bz_str_lastindexof(const void* self_p, const void* sub_p) {
    if (!self_p || !sub_p) return -1;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)sub_p;
    if (n->len == 0) return (int32_t)s->len;  // convention: last+1 position
    if (n->len > s->len) return -1;
    int64_t limit = s->len - n->len;
    for (int64_t i = limit; i >= 0; --i) {
        if (bz_str_cp_seq_eq(s, i, n, 0, n->len)) return (int32_t)i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// stdlib §3.3 / #971 item 1: ReadOnlyStringSpan search + compare family.
//
// A ReadOnlyStringSpan is {Source: string, Start: int, Length: int} — a view
// over the code-point range [Start, Start+Length) of `Source`. The search
// methods therefore need the SAME code-point comparison the whole-string
// family above uses, but over a caller-selected SUB-RANGE of each operand,
// which is exactly what makes them a separate entry point rather than a call
// into bz_str_indexof (that one always searches the whole string).
//
// Every operand is passed as an explicit (string, start, len) triple: the
// receiver span, and — for the substring forms — the needle span. The `string`
// overloads of §3.3 ("equivalent to calling the ReadOnlyStringSpan overload
// after value.AsSpan()") are the same call with
// (needle, 0, BZ_STRSPAN_WHOLE), so no separate entry point is needed for them.
//
// Comparison is code-point-by-code-point via bz_str_cp_seq_eq, so the two
// operands' storage widths are irrelevant and may differ (§4.6 adaptive
// storage — the reason ReadOnlyStringSpan is a distinct type from
// ReadOnlySpan<char>, per §3.3 "Why a separate type"). No normalization and no
// case folding, per §3.3 "Prefix and suffix" / "Equality".
//
// All indices and lengths are CODE-POINT counts. Returned indices are relative
// to the RECEIVER SPAN's start (§3.3: "the start index of the match in the
// receiver"), not to the underlying source string.
//
// The `len == 0` needle conventions mirror the whole-string family above so the
// span and string surfaces agree: an empty needle is found at index 0
// (bz_str_indexof), is always contained (bz_str_contains), and is both a prefix
// and a suffix of every span (bz_str_startswith / bz_str_endswith).
//
// Defensive clamps: the codegen bounds-checks span construction, so a negative
// start/len cannot arise from Bezel source; the guards here keep a stray
// runtime caller from reading outside the source buffer.
// ---------------------------------------------------------------------------

// Normalize a (start, len) pair against the source string's code-point length,
// so no helper below can index outside `s`'s data buffer.
//
// `len == BZ_STRSPAN_WHOLE` means "the rest of the source from `start`" — the
// same sentinel bz_str_substring uses for its one-arg overload. Codegen passes
// it for a `string` OPERAND (the §3.3 convenience overloads, whose span is the
// whole string): resolving the length HERE, after each entry point's null
// guard, is what keeps codegen from having to dereference the operand to read
// its length. That matters because the builtin string-method surface currently
// admits a `string?` argument without narrowing (`"h".IndexOf(nullString)`
// compiles and returns -1 through bz_str_indexof's own null guard), so a null
// operand does reach these functions and must behave the same way rather than
// fault. Any OTHER negative length is nonsense and clamps to empty.
#define BZ_STRSPAN_WHOLE ((int64_t)-1)
static inline void bz_strspan_clamp(const bz_string_struct* s, int64_t* start,
                                     int64_t* len) {
    if (*start < 0) *start = 0;
    if (*start > s->len) *start = s->len;
    int64_t avail = s->len - *start;
    if (*len == BZ_STRSPAN_WHOLE) *len = avail;
    else if (*len < 0)            *len = 0;
    if (*len > avail)             *len = avail;
}

// §3.3: IndexOf(value: char): int — first index in the span whose code point
// equals `cp`, or -1 if none.
extern "C" int32_t bz_strspan_indexof_cp(const void* self_p, int64_t s_start,
                                          int64_t s_len, int32_t cp) {
    if (!self_p) return -1;
    const auto* s = (const bz_string_struct*)self_p;
    bz_strspan_clamp(s, &s_start, &s_len);
    for (int64_t i = 0; i < s_len; ++i) {
        if (bz_str_cp(s, s_start + i) == (uint32_t)cp) return (int32_t)i;
    }
    return -1;
}

// §3.3: IndexOf(value: ReadOnlyStringSpan): int — first occurrence of the
// needle span as a contiguous substring of the receiver span. Naïve scan (§3.3
// explicitly leaves the algorithm to the implementation). Returns the match's
// start index RELATIVE TO THE RECEIVER SPAN, or -1 if not found.
extern "C" int32_t bz_strspan_indexof(const void* self_p, int64_t s_start,
                                       int64_t s_len, const void* other_p,
                                       int64_t o_start, int64_t o_len) {
    if (!self_p || !other_p) return -1;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)other_p;
    bz_strspan_clamp(s, &s_start, &s_len);
    bz_strspan_clamp(n, &o_start, &o_len);
    if (o_len == 0) return 0;          // empty needle matches at 0 (§9.6 form)
    if (o_len > s_len) return -1;
    int64_t limit = s_len - o_len;
    for (int64_t i = 0; i <= limit; ++i) {
        if (bz_str_cp_seq_eq(s, s_start + i, n, o_start, o_len))
            return (int32_t)i;
    }
    return -1;
}

// §3.3: StartsWith(prefix): bool — the needle matches the leading portion.
extern "C" int32_t bz_strspan_startswith(const void* self_p, int64_t s_start,
                                          int64_t s_len, const void* other_p,
                                          int64_t o_start, int64_t o_len) {
    if (!self_p || !other_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)other_p;
    bz_strspan_clamp(s, &s_start, &s_len);
    bz_strspan_clamp(n, &o_start, &o_len);
    if (o_len > s_len) return 0;
    return bz_str_cp_seq_eq(s, s_start, n, o_start, o_len);
}

// §3.3: EndsWith(suffix): bool — the needle matches the trailing portion.
extern "C" int32_t bz_strspan_endswith(const void* self_p, int64_t s_start,
                                        int64_t s_len, const void* other_p,
                                        int64_t o_start, int64_t o_len) {
    if (!self_p || !other_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)other_p;
    bz_strspan_clamp(s, &s_start, &s_len);
    bz_strspan_clamp(n, &o_start, &o_len);
    if (o_len > s_len) return 0;
    return bz_str_cp_seq_eq(s, s_start + (s_len - o_len), n, o_start, o_len);
}

// §3.3: SequenceEqual(other): bool — same length AND matching code points at
// every position. Pairwise; no normalization, no case folding.
extern "C" int32_t bz_strspan_seqequal(const void* self_p, int64_t s_start,
                                        int64_t s_len, const void* other_p,
                                        int64_t o_start, int64_t o_len) {
    if (!self_p || !other_p) return 0;
    const auto* s = (const bz_string_struct*)self_p;
    const auto* n = (const bz_string_struct*)other_p;
    bz_strspan_clamp(s, &s_start, &s_len);
    bz_strspan_clamp(n, &o_start, &o_len);
    if (s_len != o_len) return 0;
    return bz_str_cp_seq_eq(s, s_start, n, o_start, o_len);
}

// per §9.5 / §4.6 (issue #1282): char.ToString() / interpolation of a char. A
// Bezel char is a Unicode scalar (up to U+10FFFF); build a single-code-point
// string at the narrowest storage width (1/2/4 bytes per §4.6 adaptive
// storage) so the full code point survives — the old codegen truncated it to
// its low byte (`%c`), corrupting any char above U+00FF. On output the string
// is UTF-8-encoded by bz_string_to_utf8z, so `char(0x0141)` renders as its
// UTF-8 form. Per §9.6: this helper is the runtime backing for the implicit
// string conversion of a char.
extern "C" void* bz_char_to_string(int32_t cp) {
    uint32_t u = (uint32_t)cp;
    int8_t w = (u <= 0xFF) ? 1 : (u <= 0xFFFF ? 2 : 4);
    void* data = calloc((size_t)w * 2u, 1);  // one code point + a width-sized NUL
    if (w == 1)      ((uint8_t*)data)[0]  = (uint8_t)u;
    else if (w == 2) ((uint16_t*)data)[0] = (uint16_t)u;
    else             ((uint32_t*)data)[0] = u;
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    int64_t one = 1;
    memcpy(strObj + 8, &one, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// fix #4420 (stdlib §9.5, v0.306): char classification / simple-case-mapping,
// lowered here ONCE and shared by both call sites that used to hit
// <ctype.h> directly (LlvmBackend_Calls2.cpp's char.Is*/To* dispatch, and
// string.IsNullOrWhiteSpace's per-code-point loop). C11 §7.4p1 requires a
// <ctype.h> argument to be representable as `unsigned char` (0-255) or equal
// to `EOF`; a Bezel char is a 32-bit Unicode scalar up to U+10FFFF (§4.5), so
// the old code's raw `isalpha(cp)` etc. was undefined for any code point
// above U+00FF (typical glibc indexes a 384-entry table off the argument —
// an out-of-bounds read returning a plausible-looking bool, not merely a
// wrong one).
//
// §9.5 declares every `Is*` predicate TOTAL over the Unicode general
// category ("each char gets an answer, and none of them raises"). Two of
// the eight predicates are exactly decidable for the FULL char domain
// without any Unicode Character Database table, because their category is
// a small, closed, version-stable list — not a slice of a much larger
// table:
//   - IsControl / Cc is *defined* as exactly the C0 and C1 control codes
//     (U+0000-U+001F, U+007F, U+0080-U+009F) and nothing else.
//   - IsWhiteSpace / the Unicode White_Space property is the ~25-code-point
//     PropList.txt list (ASCII whitespace + NEL/NBSP/line & paragraph
//     separators/the various Zs spaces), long-stable.
// bz_char_is_control / bz_char_is_white_space implement those closed lists
// directly and do not consult libc at all.
//
// The remaining six (IsDigit/Nd, IsLetter/L*, IsLetterOrDigit, IsUpper/Lu,
// IsLower/Ll, IsPunctuation/P*) and the two case-mapping members
// (ToUpper/ToLower) span thousands of code points scattered across nearly
// every Unicode block (Arabic-Indic digits, CJK punctuation, per-script
// case pairs, …) — there is no small closed list, so they are only
// decidable within ASCII (0x00-0x7F), where the "C"-locale libc answer IS
// definitionally the Unicode general-category answer (verified: glibc's
// "C"-locale ctype tables return false for isalpha/isdigit/isalnum/isupper/
// islower/ispunct/isspace for EVERY code point above 0x7F, so restricting
// the libc call to ASCII changes nothing about the pre-fix answer for
// 0x80-0xFF and removes the UB for 0x100+ at the same time). Above ASCII
// these report the domain-safe default — `false` for the predicates,
// the code point unchanged for ToUpper/ToLower, consistent with §9.5's own
// "return the argument unchanged where it has [no case mapping]" — rather
// than guessing from an incomplete slice of a category table (issue #4420:
// "do not close this by widening a category table"). Full correctness above
// ASCII needs Unicode Character Database data the runtime does not embed;
// that is left as a follow-up, not invented here.
static inline int bz_char_is_ascii_domain(int32_t cp) {
    return cp >= 0 && cp <= 0x7F;
}

extern "C" int32_t bz_char_is_control(int32_t cp) {
    uint32_t u = (uint32_t)cp;
    return (u <= 0x1F) || (u == 0x7F) || (u >= 0x80 && u <= 0x9F);
}

extern "C" int32_t bz_char_is_white_space(int32_t cp) {
    switch ((uint32_t)cp) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: // TAB..CR
        case 0x20:                                             // SPACE
        case 0x85:                                             // NEL
        case 0xA0:                                             // NO-BREAK SPACE
        case 0x1680:                                           // OGHAM SPACE MARK
        case 0x2000: case 0x2001: case 0x2002: case 0x2003:
        case 0x2004: case 0x2005: case 0x2006: case 0x2007:
        case 0x2008: case 0x2009: case 0x200A:  // EN QUAD .. HAIR SPACE
        case 0x2028:                                           // LINE SEPARATOR
        case 0x2029:                                           // PARAGRAPH SEPARATOR
        case 0x202F:                                           // NARROW NO-BREAK SPACE
        case 0x205F:                                           // MEDIUM MATHEMATICAL SPACE
        case 0x3000:                                           // IDEOGRAPHIC SPACE
            return 1;
        default:
            return 0;
    }
}

extern "C" int32_t bz_char_is_digit(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? (isdigit(cp) != 0) : 0;
}
extern "C" int32_t bz_char_is_letter(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? (isalpha(cp) != 0) : 0;
}
extern "C" int32_t bz_char_is_letter_or_digit(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? (isalnum(cp) != 0) : 0;
}
extern "C" int32_t bz_char_is_upper(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? (isupper(cp) != 0) : 0;
}
extern "C" int32_t bz_char_is_lower(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? (islower(cp) != 0) : 0;
}
extern "C" int32_t bz_char_is_punctuation(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? (ispunct(cp) != 0) : 0;
}
extern "C" int32_t bz_char_to_upper(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? toupper(cp) : cp;
}
extern "C" int32_t bz_char_to_lower(int32_t cp) {
    return bz_char_is_ascii_domain(cp) ? tolower(cp) : cp;
}

// fix #4483 (stdlib §9.6, v0.306): string.Equals(other, ignoreCase: true) —
// ordinal, code-point-wise, case-folded equality.
//
// This replaces a codegen-inlined "case-fold each side into a fresh buffer,
// then compare the buffers" loop (LlvmBackend_String.cpp) that carried TWO
// independent defects at one site:
//
//   1. It called <ctype.h>'s tolower() on a code point produced by the
//      WIDTH-AWARE decoder, so anything above U+00FF was passed outside
//      tolower's domain — undefined per C11 §7.4p1, the same defect class
//      #4420 removed from char.* and string.IsNullOrWhiteSpace. ASan cannot
//      see it (glibc's ctype tables are uninstrumented libc), and on glibc
//      tolower happens to range-check and hand the argument back, which is
//      exactly why it survived: benign HERE, on THIS libc, still undefined.
//
//   2. It truncated the folded code point to ONE BYTE and stored it at a
//      hardcoded stride of 1 while the source index advanced one CODE POINT
//      at a time, then labelled the result width=1 (§4.6). So every code
//      point above U+00FF was silently replaced by its low byte and distinct
//      strings compared EQUAL. Measured on HEAD before this fix:
//      "š".Equals("a", true) → true (U+0161's low byte is 0x61 = 'a'),
//      "š".Equals("ɡ", true) → true (U+0161 and U+0261 share low byte 0x61),
//      "á".Equals("ǡ", true) → true (U+00E1 and U+01E1 share low byte 0xE1).
//      All three must be false.
//
// Folding per code point decides the comparison position by position, so no
// buffer is materialised at all — which also removes the two allocations per
// operand (data + header) the old lowering never disposed (76 bytes in 4
// allocations for a single call, measured; the `ignoreCase := false` arm
// leaked nothing). That is a consequence of deleting the buffer, not a leak
// fix undertaken here (#4482 owns this area's disposal gaps).
//
// The fold is bz_char_to_lower — the SAME shared helper char.ToLower and the
// rest of the #4420 family use — so the runtime holds exactly one case
// policy; two copies of a classification rule drifting apart is #2590. Within
// ASCII that is precisely the simple case fold §9.6's `ignoreCase` overloads
// specify; above ASCII it is identity, so two code points above ASCII compare
// equal iff they ARE the same code point. That is #4420's domain-safe answer
// and deliberately NOT a widened category table: full simple case folding
// above ASCII needs Unicode Character Database data this runtime does not
// embed, and #4481 tracks that (no-UB, wrong-value-only) range.
//
// The operands' storage widths are irrelevant and may differ (§4.6 adaptive
// storage): bz_str_cp decodes each side at its own width, exactly as the
// bz_str_* search family above does. Either pointer may be null (null equals
// only null), mirroring emitStringEq's null-safety on the `=` path.
extern "C" int32_t bz_str_equals_ignorecase(const void* self_p, const void* other_p) {
    if (!self_p || !other_p) return self_p == other_p ? 1 : 0;
    const auto* a = (const bz_string_struct*)self_p;
    const auto* b = (const bz_string_struct*)other_p;
    if (a->len != b->len) return 0;               // §9.6: lengths in code points
    for (int64_t i = 0; i < a->len; ++i) {
        if (bz_char_to_lower((int32_t)bz_str_cp(a, i)) !=
            bz_char_to_lower((int32_t)bz_str_cp(b, i)))
            return 0;
    }
    return 1;
}

// fix #4487 (stdlib §9.6 + spec §4.6, v0.306): the ONE ordinal string
// comparison — `CompareTo(other)`, `CompareTo(other, ignoreCase)`, and the
// `<` / `<=` / `>` / `>=` operators, all of which the two documents define as
// the SAME relation.
//
// Spec §4.6 (v0.306): "`s < t`, `s > t`, `s <= t`, `s >= t` perform ordinal
// lexicographic comparison of the code-point sequence." Stdlib §9.6 (v0.306):
// "All comparison and equality methods in this version are ordinal (code-
// point-based), including the `ignoreCase` overloads". Stdlib §2.8: the result
// is negative / zero / positive — its MAGNITUDE is not specified, so this
// returns -1 / 0 / +1.
//
// It replaces `bz_string_compare(a_data, a_len, b_data, b_len, ignore_case)`,
// which this commit deletes. That helper could not be repaired in place: its
// signature takes a raw data pointer and a length and NOT the storage width,
// so it was structurally unable to answer the question its callers asked. Its
// callers handed it `loadStringData` (the raw buffer) and `loadStringLen` (the
// CODE-POINT count), and it walked `min(a_len, b_len)` BYTES — the #535 defect
// class. For a width-2 string it therefore read only the first `len` of the
// `len*width` data bytes, one byte at a time. Measured on HEAD (4d9d186a)
// before this fix:
//
//     "日本".CompareTo("日x", true)  ->   0   two DIFFERENT strings, EQUAL
//     "š".CompareTo("a", true)       ->   0   U+0161 vs 'a', EQUAL
//     "š".CompareTo("ɡ", true)       ->   0   U+0161 vs U+0261, EQUAL
//
// (For `"日本"` vs `"日x"`, both width 2: data bytes `E5 65 2C 67` against
// `E5 65 78 00`, compared over n = 2 BYTES, are both `E5 65`, and the lengths
// are equal.) Unlike #4483 there is no UB here — `bz_string_compare`'s
// `tolower` calls were on `(unsigned char)` values and in-domain — only wrong
// values.
//
// The 1-arg form did not route through that helper at all: it was an inlined
// `memcmp(dataL, dataR, minLen*width)` fast path in `emitStringOrdCmp`
// (LlvmBackend_String.cpp), correct only at width 1. At width 2/4 it ordered
// by the LITTLE-ENDIAN storage bytes, i.e. by low byte first — which §4.6
// forbids outright, since the per-string width is invisible to the language
// and no result may depend on it. Measured on the same HEAD:
//
//     "日本".CompareTo("日x")  -> NEGATIVE; U+672C > U+0078, so it must be > 0
//     "Āa" < "ĀĀ"              -> false  (0x61 < 0x100, so it must be true)
//
// MAGNITUDE CAVEAT: only the SIGN of that is a reproducible measurement, and
// the sign is the whole defect. glibc's `memcmp` is required to return a
// value whose sign orders the buffers; it is free to return the byte
// difference or just the sign, and the two measurements taken of this program
// disagree — -76 on the box this was first reproduced on (the analytic byte
// difference 0x2C - 0x78) and -1 on a reviewer's rebuild. NEGATIVE-where-
// §4.6-requires-POSITIVE reproduces on both. Nothing below asserts a
// magnitude; stdlib §2.8 does not specify one.
//
// Decoding per code point via `bz_str_cp` — the same width-aware reader the
// rest of the `bz_str_*` family uses — decides both forms at each position, so
// each operand is read at its OWN width and mixed widths need no special case.
// The `ignoreCase` fold is `bz_char_to_lower`, the SAME shared helper #4420
// introduced and #4483 reused, so the runtime holds exactly one case policy:
// the surviving second copy is what the #4483 reviewer flagged here, and two
// copies of a classification rule drifting apart is what caused #2590. Within
// ASCII that is the simple case fold §9.6 specifies; above ASCII it is
// identity, so two code points above ASCII compare equal iff they ARE the same
// code point (#4481 owns widening that, and this commit does not).
//
// Comparison is on `uint32_t` code points, so it is by scalar value (§4.5: a
// char is a Unicode scalar up to U+10FFFF, always non-negative).
//
// NULL OPERANDS are treated as the EMPTY string, deliberately reproducing
// `bz_string_compare`'s documented behaviour (`if (!a) a_len = 0;`) so that
// every moved call site keeps the contract it was written against. §4.6 says
// ordering requires non-nullable operands, so a well-typed program never
// reaches this; note it is NOT `bz_str_equals_ignorecase`'s "null equals only
// null" rule, which answers an EQUALITY question where null is a value.
extern "C" int32_t bz_str_compare_cp(const void* self_p, const void* other_p,
                                     int32_t ignore_case) {
    const auto* a = (const bz_string_struct*)self_p;
    const auto* b = (const bz_string_struct*)other_p;
    int64_t a_len = a ? a->len : 0;
    int64_t b_len = b ? b->len : 0;
    int64_t n = a_len < b_len ? a_len : b_len;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t ca = bz_str_cp(a, i);
        uint32_t cb = bz_str_cp(b, i);
        if (ignore_case) {
            ca = (uint32_t)bz_char_to_lower((int32_t)ca);
            cb = (uint32_t)bz_char_to_lower((int32_t)cb);
        }
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a_len == b_len) return 0;                 // §9.6: lengths in code points
    return a_len < b_len ? -1 : 1;
}

// per §9.6 / #535: Substring(start, count): owned string — code-point slice.
// Returns a fresh +1 string of `count` code points starting at code-point
// index `start`. Clamps `count` to the available tail if needed.
// Preserves the source string's width (§4.6 per-string storage width).
// The codegen-side bounds check (LlvmBackend_String.cpp "Substring" branch)
// raises IndexOutOfRangeException for `start < 0`, `start > len`, or
// `start + length > len`; the defensive clamps here are a backstop so a stray
// runtime caller can never read out of the source buffer. The single-arg
// overload passes `count = -1`, which the runtime expands to the rest of
// the string (start .. len).
extern "C" void* bz_str_substring(const void* self_p, int64_t start, int64_t count) {
    if (!self_p) {
        // Return empty string.
        uint8_t* e = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        void* edata = calloc(1, 1);
        memcpy(e, &edata, sizeof(void*));
        int64_t z = 0;
        memcpy(e + 8, &z, sizeof(int64_t));
        e[16] = 1;
        bz_string_init_rc(e);
        return e;
    }
    const auto* s = (const bz_string_struct*)self_p;
    if (start < 0) start = 0;
    if (start > s->len) start = s->len;
    int64_t avail = s->len - start;
    if (count < 0 || count > avail) count = avail;
    return bz_str_slice_cp(s, start, count);
}

// per §9.6 / #535: Replace(old, new): owned string — code-point-aware.
// Replaces every non-overlapping occurrence of `old_p` in `self_p` with
// `new_p`. The result may have a DIFFERENT width from the source (e.g.
// replacing ASCII content with non-ASCII widens; the output uses the narrowest
// width that fits all resulting code points). For simplicity, the result is
// built into a code-point buffer at width 4, then repacked at narrowest width.
extern "C" void* bz_str_replace(const void* self_p,
                                 const void* old_p,
                                 const void* new_p) {
    if (!self_p) {
        // Return empty string (same as bz_str_substring empty case).
        uint8_t* e = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        void* edata = calloc(1, 1);
        memcpy(e, &edata, sizeof(void*));
        int64_t z = 0;
        memcpy(e + 8, &z, sizeof(int64_t));
        e[16] = 1;
        bz_string_init_rc(e);
        return e;
    }
    const auto* s   = (const bz_string_struct*)self_p;
    const auto* old = (const bz_string_struct*)old_p;
    const auto* nw  = (const bz_string_struct*)new_p;

    // Empty needle: return a copy of self (§9.6: no-op on empty old).
    if (!old || old->len == 0) {
        return bz_str_slice_cp(s, 0, s->len);
    }

    // Collect result code points into a width-4 scratch buffer.
    // Worst-case: each source CP replaced by nw->len CPs.
    int64_t cap = (s->len + 1) * (nw ? nw->len + 1 : 1) + 1;
    uint32_t* out = (uint32_t*)malloc((size_t)cap * sizeof(uint32_t));
    int64_t olen = 0;
    uint32_t maxCp = 0;

    int64_t i = 0;
    while (i <= s->len - old->len) {
        if (bz_str_cp_seq_eq(s, i, old, 0, old->len)) {
            // Emit replacement.
            if (nw) {
                for (int64_t k = 0; k < nw->len; ++k) {
                    uint32_t cp = bz_str_cp(nw, k);
                    if (cp > maxCp) maxCp = cp;
                    out[olen++] = cp;
                }
            }
            i += old->len;
        } else {
            uint32_t cp = bz_str_cp(s, i);
            if (cp > maxCp) maxCp = cp;
            out[olen++] = cp;
            ++i;
        }
    }
    // Emit remaining tail.
    while (i < s->len) {
        uint32_t cp = bz_str_cp(s, i);
        if (cp > maxCp) maxCp = cp;
        out[olen++] = cp;
        ++i;
    }

    // Pack into narrowest width (§4.6 adaptive storage).
    int8_t w = (maxCp <= 0xFF) ? 1 : (maxCp <= 0xFFFF ? 2 : 4);
    void* data = calloc((size_t)(olen > 0 ? olen : 1) * (size_t)w + (size_t)w, 1);
    if (w == 1) {
        uint8_t* d = (uint8_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = (uint8_t)out[k];
    } else if (w == 2) {
        uint16_t* d = (uint16_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = (uint16_t)out[k];
    } else {
        uint32_t* d = (uint32_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = out[k];
    }
    free(out);

    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &olen, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// per §9.6 / #535: Split(separator: char) — code-point-aware single separator.
// Splits `self_p` at every code point that equals `sep` (a Unicode scalar).
// Returns an owned bz_array_t* of bz.string*; each segment preserves the
// source string's width. Supersedes bz_string_split_anychar for the char
// overload (which passed raw byte data + code-point count, wrong for width>1).
extern "C" void* bz_str_split_cp(const void* self_p, int32_t sep) {
    void* arr = bz_array_alloc(8, 8);
    if (!self_p) {
        // Return array with one empty-string element.
        void* e = bz_str_substring(nullptr, 0, 0);
        bz_array_add(arr, &e, 8);
        return arr;
    }
    const auto* s = (const bz_string_struct*)self_p;
    int64_t start = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        if ((int32_t)bz_str_cp(s, i) == sep) {
            void* seg = bz_str_slice_cp(s, start, i - start);
            bz_array_add(arr, &seg, 8);
            start = i + 1;
        }
    }
    // Trailing segment (possibly empty).
    void* seg = bz_str_slice_cp(s, start, s->len - start);
    bz_array_add(arr, &seg, 8);
    return arr;
}

// per §9.6 / #535: Split(separator: string) — code-point-aware string delimiter.
// Splits `self_p` at every non-overlapping occurrence of `delim_p`.
// Returns an owned bz_array_t* of bz.string*; each segment preserves the
// source string's width.
extern "C" void* bz_str_split_str(const void* self_p, const void* delim_p) {
    void* arr = bz_array_alloc(8, 8);
    if (!self_p) {
        void* e = bz_str_substring(nullptr, 0, 0);
        bz_array_add(arr, &e, 8);
        return arr;
    }
    const auto* s = (const bz_string_struct*)self_p;
    if (!delim_p) {
        void* seg = bz_str_slice_cp(s, 0, s->len);
        bz_array_add(arr, &seg, 8);
        return arr;
    }
    const auto* d = (const bz_string_struct*)delim_p;
    if (d->len == 0) {
        // Empty delimiter: return array with one element (the whole string).
        void* seg = bz_str_slice_cp(s, 0, s->len);
        bz_array_add(arr, &seg, 8);
        return arr;
    }
    int64_t start = 0;
    int64_t limit = s->len - d->len;
    for (int64_t i = 0; i <= limit; ) {
        if (bz_str_cp_seq_eq(s, i, d, 0, d->len)) {
            void* seg = bz_str_slice_cp(s, start, i - start);
            bz_array_add(arr, &seg, 8);
            start = i + d->len;
            i = start;
        } else {
            ++i;
        }
    }
    // Trailing segment.
    void* seg = bz_str_slice_cp(s, start, s->len - start);
    bz_array_add(arr, &seg, 8);
    return arr;
}

// stdlib §9.6 (#741): is a code point in the char-set array? `set` is an
// Array<char> (bz_array_t of i32 code points). A null/empty set matches nothing.
static inline bool bz_cp_in_set(const bz_array_t* set, int32_t cp) {
    if (!set || !set->data) return false;
    const int32_t* d = (const int32_t*)set->data;
    for (int64_t i = 0; i < set->count; ++i)
        if (d[i] == cp) return true;
    return false;
}

// stdlib §9.6 (#741): Split(separators: Array<char>) — split at ANY code point
// in `seps`. Mirrors bz_str_split_cp but tests set membership. An empty/null
// separator set yields the whole string as one segment. Owned Array<string>.
extern "C" void* bz_str_split_chars(const void* self_p, const void* seps_p) {
    void* arr = bz_array_alloc(8, 8);
    const auto* s = (const bz_string_struct*)self_p;
    const auto* seps = (const bz_array_t*)seps_p;
    if (!s) {
        void* e = bz_str_substring(nullptr, 0, 0);
        bz_array_add(arr, &e, 8);
        return arr;
    }
    if (!seps || seps->count == 0) {
        void* seg = bz_str_slice_cp(s, 0, s->len);
        bz_array_add(arr, &seg, 8);
        return arr;
    }
    int64_t start = 0;
    for (int64_t i = 0; i < s->len; ++i) {
        if (bz_cp_in_set(seps, (int32_t)bz_str_cp(s, i))) {
            void* seg = bz_str_slice_cp(s, start, i - start);
            bz_array_add(arr, &seg, 8);
            start = i + 1;
        }
    }
    void* seg = bz_str_slice_cp(s, start, s->len - start);
    bz_array_add(arr, &seg, 8);
    return arr;
}

// stdlib §9.6 (#741): Trim(chars: IEnumerable<char>) — strip leading & trailing
// code points that are in the `chars` set (passed as Array<char>). Returns a
// fresh bz_string. A null/empty set returns the whole string unchanged.
extern "C" void* bz_str_trim_chars(const void* self_p, const void* chars_p) {
    const auto* s = (const bz_string_struct*)self_p;
    const auto* set = (const bz_array_t*)chars_p;
    if (!s) return bz_str_substring(nullptr, 0, 0);
    int64_t start = 0, end = s->len;
    if (set && set->count > 0) {
        while (start < end &&
               bz_cp_in_set(set, (int32_t)bz_str_cp(s, start))) ++start;
        while (end > start &&
               bz_cp_in_set(set, (int32_t)bz_str_cp(s, end - 1))) --end;
    }
    return bz_str_slice_cp(s, start, end - start);
}

// stdlib §9.6 (#7492): Trim(chars: IEnumerable<char>) where the `chars` argument
// is a `string`. A `string` is an `IEnumerable<char>` (stdlib §9.6), so
// `s.Trim("ab")` strips the code points 'a' and 'b'. `bz_str_trim_chars` above
// reads its set as a `bz_array_t` (an `Array<char>` of i32 code points); a
// `string` is a `bz_string_struct` -- `set->count` read a field that is not a
// count and the trim loop ran off the end of the string. The codegen therefore
// selects THIS entry by the argument's static type. The set is walked in the
// CODE-POINT domain (`bz_str_cp`), so the set string's own storage width
// (§4.6: 1/2/4 bytes per code point) is never observable.
extern "C" void* bz_str_trim_chars_str(const void* self_p, const void* set_p) {
    const auto* s = (const bz_string_struct*)self_p;
    const auto* set = (const bz_string_struct*)set_p;
    if (!s) return bz_str_substring(nullptr, 0, 0);
    int64_t start = 0, end = s->len;
    auto inSet = [&](uint32_t cp) {
        for (int64_t i = 0; i < set->len; ++i)
            if (bz_str_cp(set, i) == cp) return true;
        return false;
    };
    if (set && set->len > 0) {
        while (start < end && inSet(bz_str_cp(s, start))) ++start;
        while (end > start && inSet(bz_str_cp(s, end - 1))) --end;
    }
    return bz_str_slice_cp(s, start, end - start);
}

// §9.6 / issue #654: string.ToCharArray(): owned Array<char>.
// Returns a fresh bz_array_t* with elem_size=4 (char = i32 code point).
// Each element is the i32 Unicode code point at the corresponding position.
// The returned array is owned: callers must dispose it (bz_array_dispose).
extern "C" void* bz_str_to_char_array(const void* self_p) {
    // elem_size=4: each char slot holds a 32-bit code point (int32_t).
    void* arr = bz_array_alloc(4, 0);
    if (!self_p) return arr;  // null string → empty array
    const auto* s = (const bz_string_struct*)self_p;
    for (int64_t i = 0; i < s->len; ++i) {
        int32_t cp = (int32_t)bz_str_cp(s, i);
        bz_array_add(arr, &cp, 4);
    }
    return arr;
}

// §9.6 (#747 R43): string.From(buffer: Array<char>, start, count): string.
// Inverse of ToCharArray — builds a fresh +1 string from the i32 code points at
// the char array's slots [start, start+count). The char array stores one i32
// code point per slot (elem_size=4, matching bz_str_to_char_array). The result
// uses the narrowest §4.6 adaptive width that fits all code points. Bounds are
// validated in codegen (IndexOutOfRangeException); the clamps here are a
// defensive backstop so a stray call can never read out of the array buffer.
extern "C" void* bz_str_from_char_array(const void* arr_p,
                                        int64_t start, int64_t count) {
    const auto* arr = (const bz_array_t*)arr_p;
    int64_t n = arr ? arr->count : 0;
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (count < 0) count = 0;
    if (start + count > n) count = n - start;
    const int32_t* src = arr ? (const int32_t*)arr->data : nullptr;
    int64_t olen = count;
    uint32_t maxCp = 0;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(olen > 0 ? olen : 1) * sizeof(uint32_t));
    for (int64_t k = 0; k < olen; ++k) {
        uint32_t cp = (uint32_t)src[start + k];
        if (cp > maxCp) maxCp = cp;
        out[k] = cp;
    }
    // Pack into the narrowest width (§4.6), mirroring bz_str_replace's repack.
    int8_t w = (maxCp <= 0xFF) ? 1 : (maxCp <= 0xFFFF ? 2 : 4);
    void* data =
        calloc((size_t)(olen > 0 ? olen : 1) * (size_t)w + (size_t)w, 1);
    if (w == 1) {
        uint8_t* d = (uint8_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = (uint8_t)out[k];
    } else if (w == 2) {
        uint16_t* d = (uint16_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = (uint16_t)out[k];
    } else {
        uint32_t* d = (uint32_t*)data;
        for (int64_t k = 0; k < olen; ++k) d[k] = out[k];
    }
    free(out);
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &olen, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// Write one code point into the `index`-th slot of a packed width-wide buffer.
static inline void bz_put_cp(void* data, int64_t index, int8_t width, uint32_t cp) {
    if (width == 1)      ((uint8_t*)data)[index]  = (uint8_t)cp;
    else if (width == 2) ((uint16_t*)data)[index] = (uint16_t)cp;
    else                 ((uint32_t*)data)[index] = cp;
}

// The storage width a code point needs (spec §4.6: "typically 1, 2, or 4 bytes
// per code point, selected at construction based on the largest code point in
// the value"). The narrowest width that holds the code point.
static inline int8_t bz_cp_width(uint32_t cp) {
    if (cp <= 0xFFu)   return 1;
    if (cp <= 0xFFFFu) return 2;
    return 4;
}

// The storage width a code-point SEQUENCE is packed at (spec §4.6): the
// narrowest width that fits its largest code point.
static int8_t bz_cps_width(const uint32_t* cps, int64_t count) {
    uint32_t maxCp = 0;
    for (int64_t i = 0; i < count; i++)
        if (cps[i] > maxCp) maxCp = cps[i];
    return bz_cp_width(maxCp);
}

// ---------------------------------------------------------------------------
// #7425 (§4.6, v0.345): the §9.6 transform surface, in the CODE-POINT domain.
//
// The codegen arms for these members used to work in the BYTE domain: they
// allocated `len + 1` BYTES for a string whose data is `len * width` bytes,
// copied `len` bytes (not `len * width`), compared / folded / trimmed one BYTE
// at a time, and stored width = 1. On a width-2 or width-4 string that is
// wrong twice over, and §4.6 (v0.345) is explicit that the width is not
// observable: "The choice of width is invisible at the language level;
// programs observe only the `char` sequence and its length."
//
//   * `Remove` allocated `resultLen + 1` bytes but wrote `resultLen * width`
//     of them — an ASan heap-buffer-overflow WRITE ("WRITE of size 1 … 0
//     bytes after a 2-byte region") on every width >= 2;
//   * the others produced a string whose CONTENT is the byte sequence, not
//     the code-point sequence — the wrong text at the right length.
//
// The helpers below do the work in the code-point domain, like the width-aware
// helpers already in this file (bz_str_replace, bz_str_pad, bz_str_substring).

// Build a fresh +1 bz_string from a code-point sequence, packed at the
// NARROWEST width that fits its largest code point (§4.6 adaptive storage —
// the same rule bz_str_replace and bz_str_slice_cp use).
static void* bz_str_pack_cps(const uint32_t* cps, int64_t count) {
    int8_t w = bz_cps_width(cps, count);
    void* data = calloc((size_t)(count > 0 ? count : 1) * (size_t)w + (size_t)w,
                         1);  // + width-sized NUL terminator slot
    for (int64_t i = 0; i < count; ++i) bz_put_cp(data, i, w, cps[i]);
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    int64_t n = count;
    memcpy(strObj + 8, &n, sizeof(int64_t));
    strObj[16] = (uint8_t)w;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    return strObj;
}

// Is this code point an ASCII whitespace code point? The trim contract the
// inline arm applied (isspace over the BYTE) is the ASCII set; a code point
// above U+00FF is not in it.
static inline bool bz_str_cp_is_space(uint32_t cp) {
    return cp <= 0xFFu && isspace((int)cp) != 0;
}

// §9.6: Remove(startIndex, length): string — remove `length` CODE POINTS
// starting at code-point index `startIndex`; a negative `length` means "to the
// end of the string" (the 1-argument `Remove(startIndex)` spelling).
//
// The codegen guard (fix #7168) has already rejected an out-of-range window
// with IndexOutOfRangeException; the clamps here are the defensive backstop
// the other width-aware helpers use, so a stray runtime caller can never read
// out of the source buffer.
extern "C" void* bz_str_remove(const void* self_p, int64_t start, int64_t count) {
    const auto* s = (const bz_string_struct*)self_p;
    int64_t len = s ? s->len : 0;
    if (start < 0) start = 0;
    if (start > len) start = len;
    int64_t avail = len - start;
    if (count < 0 || count > avail) count = avail;
    int64_t outLen = len - count;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(outLen > 0 ? outLen : 1) * sizeof(uint32_t));
    for (int64_t i = 0; i < start; ++i) out[i] = bz_str_cp(s, i);
    for (int64_t i = start + count; i < len; ++i) out[i - count] = bz_str_cp(s, i);
    void* r = bz_str_pack_cps(out, outLen);
    free(out);
    return r;
}

// §9.6: Replace(oldChar, newChar): string — replace every occurrence of the
// CODE POINT `oldCp` with the code point `newCp`. The `char` parameters are
// code points (§4.6), not bytes.
extern "C" void* bz_str_replace_char(const void* self_p, int32_t oldCp,
                                     int32_t newCp) {
    const auto* s = (const bz_string_struct*)self_p;
    int64_t len = s ? s->len : 0;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(len > 0 ? len : 1) * sizeof(uint32_t));
    for (int64_t i = 0; i < len; ++i) {
        uint32_t cp = bz_str_cp(s, i);
        out[i] = ((int32_t)cp == oldCp) ? (uint32_t)newCp : cp;
    }
    void* r = bz_str_pack_cps(out, len);
    free(out);
    return r;
}

// §9.6: ToUpper() / ToLower(): string — the case fold, per CODE POINT.
//
// `upper` selects the direction (1 = upper, 0 = lower). For a code point
// that fits a byte the fold is the one libc applies to that byte — which is
// exactly what the old BYTE-domain arm applied, so a width-1 string (whose
// code points ARE its bytes) is UNCHANGED. A code point above U+00FF has no
// single-byte case mapping and is left alone.
extern "C" void* bz_str_case(const void* self_p, int32_t upper) {
    const auto* s = (const bz_string_struct*)self_p;
    int64_t len = s ? s->len : 0;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(len > 0 ? len : 1) * sizeof(uint32_t));
    for (int64_t i = 0; i < len; ++i) {
        uint32_t cp = bz_str_cp(s, i);
        if (cp <= 0xFFu) {
            int folded = upper ? toupper((int)cp) : tolower((int)cp);
            out[i] = (uint32_t)folded;
        } else {
            out[i] = cp;
        }
    }
    void* r = bz_str_pack_cps(out, len);
    free(out);
    return r;
}

// §9.6: Trim() / TrimStart() / TrimEnd(): string — strip the ASCII whitespace
// CODE POINTS at the leading end, the trailing end, or both.
//
// `mode`: 0 = Trim (both ends), 1 = TrimStart, 2 = TrimEnd.
extern "C" void* bz_str_trim_ws(const void* self_p, int32_t mode) {
    const auto* s = (const bz_string_struct*)self_p;
    if (!s) return bz_str_pack_cps(nullptr, 0);
    int64_t start = 0, end = s->len;
    if (mode == 0 || mode == 1)
        while (start < end && bz_str_cp_is_space(bz_str_cp(s, start))) ++start;
    if (mode == 0 || mode == 2)
        while (end > start && bz_str_cp_is_space(bz_str_cp(s, end - 1))) --end;
    int64_t count = end - start;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(count > 0 ? count : 1) * sizeof(uint32_t));
    for (int64_t i = 0; i < count; ++i) out[i] = bz_str_cp(s, start + i);
    void* r = bz_str_pack_cps(out, count);
    free(out);
    return r;
}

// §9.6: Repeat(unit: string, count: int): string — the `unit` repeated
// `count` times, in the code-point domain. `count` < 0 is rejected by the
// codegen guard (fix #7120) before this is called; the clamp here is the
// defensive backstop.
extern "C" void* bz_str_repeat(const void* self_p, int64_t count) {
    const auto* s = (const bz_string_struct*)self_p;
    int64_t len = s ? s->len : 0;
    if (count < 0) count = 0;
    int64_t outLen = len * count;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(outLen > 0 ? outLen : 1) * sizeof(uint32_t));
    for (int64_t k = 0; k < count; ++k)
        for (int64_t i = 0; i < len; ++i)
            out[k * len + i] = bz_str_cp(s, i);
    void* r = bz_str_pack_cps(out, outLen);
    free(out);
    return r;
}

// §9.6: Repeat(c: char, count: int): string — a `count`-long string of the
// single code point `c`. The char is a CODE POINT (§4.6), so a code point
// above U+00FF needs a width-2 or width-4 result — the old BYTE-domain arm
// truncated it to its low byte.
extern "C" void* bz_str_repeat_char(int32_t cp, int64_t count) {
    if (count < 0) count = 0;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(count > 0 ? count : 1) * sizeof(uint32_t));
    for (int64_t i = 0; i < count; ++i) out[i] = (uint32_t)cp;
    void* r = bz_str_pack_cps(out, count);
    free(out);
    return r;
}

// §9.6-ext: GetField(delim: char, n: int): string — the n-th (0-based)
// delimiter-separated field of `self`, or "" when there is no such field.
//
// ⚠ GetField is NOT a stdlib §9.6 member (issue #7131 records that the
// compiler implements three §9.6 members the document does not declare). This
// helper therefore preserves the COMPILER's existing semantics — the n-th
// field of a split on `delim`, "" when out of range — and only moves the
// work from the byte domain into the code-point domain, like the helpers
// above. The API-existence question stays with #7131.
extern "C" void* bz_str_getfield(const void* self_p, int32_t delim, int64_t n) {
    const auto* s = (const bz_string_struct*)self_p;
    if (!s || n < 0) return bz_str_pack_cps(nullptr, 0);
    // Walk the fields, remembering where the n-th one starts and where it
    // ends. A field runs from just after the (n-1)-th delimiter to the next
    // delimiter or to the end of the string.
    int64_t start = 0;
    int64_t field = 0;
    while (field < n) {
        int64_t i = start;
        while (i < s->len && (int32_t)bz_str_cp(s, i) != delim) ++i;
        if (i >= s->len) { start = s->len; break; }  // no such field
        start = i + 1;
        ++field;
    }
    int64_t end = start;
    while (end < s->len && (int32_t)bz_str_cp(s, end) != delim) ++end;
    int64_t count = end - start;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(count > 0 ? count : 1) * sizeof(uint32_t));
    for (int64_t i = 0; i < count; ++i) out[i] = bz_str_cp(s, start + i);
    void* r = bz_str_pack_cps(out, count);
    free(out);
    return r;
}

// §9.6-ext: CountOf(sub: string): int — the number of non-overlapping
// occurrences of `sub` in `self`.
//
// ⚠ CountOf is NOT a stdlib §9.6 member (issue #7131) — the same posture as
// GetField above: the semantics are the compiler's, moved into the code-point
// domain. The base used `memmem` over the raw BYTES, which cannot see a
// code point in a width-2 or width-4 string.
extern "C" int32_t bz_str_count_of(const void* self_p, const void* sub_p) {
    const auto* s = (const bz_string_struct*)self_p;
    const auto* sub = (const bz_string_struct*)sub_p;
    if (!s || !sub || sub->len == 0) return 0;
    int32_t count = 0;
    int64_t i = 0;
    while (i + sub->len <= s->len) {
        if (bz_str_cp_seq_eq(s, i, sub, 0, sub->len)) {
            ++count;
            i += sub->len;
        } else {
            ++i;
        }
    }
    return count;
}

// §9.6: Insert(startIndex: int, value: string): string — insert `value` at
// code-point index `startIndex`. The two operands may have DIFFERENT storage
// widths (§4.6: the width is per string), so the result is assembled in the
// code-point domain and packed at the narrowest width that fits it.
extern "C" void* bz_str_insert(const void* self_p, int64_t idx,
                               const void* value_p) {
    const auto* s = (const bz_string_struct*)self_p;
    const auto* v = (const bz_string_struct*)value_p;
    int64_t len = s ? s->len : 0;
    int64_t vlen = v ? v->len : 0;
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    int64_t outLen = len + vlen;
    uint32_t* out =
        (uint32_t*)malloc((size_t)(outLen > 0 ? outLen : 1) * sizeof(uint32_t));
    for (int64_t i = 0; i < idx; ++i) out[i] = bz_str_cp(s, i);
    for (int64_t k = 0; k < vlen; ++k) out[idx + k] = bz_str_cp(v, k);
    for (int64_t i = idx; i < len; ++i) out[idx + vlen + (i - idx)] = bz_str_cp(s, i);
    void* r = bz_str_pack_cps(out, outLen);
    free(out);
    return r;
}

// The builder's content, flattened in the CODE-POINT domain (spec §4.6).
//
// §4.6: a `string` stores each code point in `width` bytes (1, 2 or 4), the
// width chosen PER STRING at construction from the largest code point in the
// value. A builder's parts are strings, each with its OWN width, so the
// builder's content is NOT the concatenation of the parts' BYTES: a width-1
// part's byte and a width-2 part's two bytes are different code points, and a
// byte-level concatenation of parts with DIFFERENT widths is not a
// code-point concatenation at all.
//
// The content is therefore flattened in the code-point domain, and the result
// is packed at ONE width — the NARROWEST width that fits the LARGEST CODE
// POINT IN THE VALUE (§4.6's own rule, and the rule this runtime's sibling
// paths already use: bz_str_slice_cp and bz_str_replace's repack both select
// the narrowest width that fits all of their code points).
//
// ⇒ THE WIDTH IS NOT INVISIBLE. §4.6 says the choice of width is invisible
// "at the language level" in the sense that a program observes the `char`
// sequence and its length — but stdlib §9.6 exposes `s.IsAscii` ("true iff
// every code point < 128"), and the runtime answers it from the width in O(1).
// A string therefore HAS to be stored at the narrowest width that fits its
// value, or two `=`-equal strings could answer IsAscii differently. The
// widest-part rule this file used before could not hold that: after a mutator
// narrowed the content, the part kept the wide width, so `sb.Append("日");
// sb.Replace("日","z")` produced a string `=`-equal to "z" whose IsAscii was
// false while "z".IsAscii was true (#7229's F1).
//
// out_cps: a malloc'd array of the content's code points (the caller frees
//          it). out_count: the content's code-point count (§4.6's length).
// The caller packs the sequence at the width bz_cps_width gives it — the
// narrowest width that fits its largest code point (§4.6).
static void bz_sb_content_cps(bz_array_t* arr, uint32_t** out_cps,
                              int64_t* out_count) {
    int64_t total = 0;
    if (arr && arr->data) {
        for (int64_t i = 0; i < arr->count; i++) {
            void* sptr = ((void**)arr->data)[i];
            if (!sptr) continue;
            auto* s = (bz_string_struct*)sptr;
            if (s->len <= 0) continue;
            total += s->len;
        }
    }
    uint32_t* cps = (uint32_t*)malloc((size_t)(total > 0 ? total : 1) *
                                       sizeof(uint32_t));
    int64_t off = 0;
    if (arr && arr->data) {
        for (int64_t i = 0; i < arr->count; i++) {
            void* sptr = ((void**)arr->data)[i];
            if (!sptr) continue;
            auto* s = (bz_string_struct*)sptr;
            for (int64_t k = 0; k < s->len; k++) cps[off++] = bz_str_cp(s, k);
        }
    }
    *out_cps = cps;
    *out_count = total;
}

// Pack a code-point sequence into a fresh +1 bz.string at `width` bytes per
// code point (§4.6). The builder takes the new part by transferring it in.
static void* bz_sb_string_from_cps(const uint32_t* cps, int64_t count,
                                   int8_t width) {
    size_t bytes = (size_t)count * (size_t)width;
    void* data = calloc(bytes + (size_t)width, 1);   // + width-sized NUL slot
    for (int64_t i = 0; i < count; i++) bz_put_cp(data, i, width, cps[i]);
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &count, sizeof(int64_t));
    strObj[16] = (uint8_t)width;
    bz_string_init_rc(strObj);                       // #422: producer ref, rc := 1
    return strObj;
}

// fix #7229 (spec §4.6 / stdlib §3.1, v0.344): flatten the builder's parts
// into ONE packed buffer of CODE POINTS.
//
// Returns the content's code-point count (§4.6's length) and fills `*out_data`
// with a malloc'd buffer holding the content's code points at `*out_width`
// bytes each, plus one width-sized NUL slot (the convention the other
// width-aware runtime helpers here use). The caller owns `*out_data`; it
// becomes the `string`'s data buffer.
//
// The width is the NARROWEST that fits the largest code point in the value —
// see bz_sb_content_cps for why (§4.6's rule, and stdlib §9.6's IsAscii,
// which the width answers in O(1)).
extern "C" int64_t bz_strbuilder_flatten(void* parts_arr, char** out_data,
                                          int8_t* out_width) {
    auto* arr = (bz_array_t*)parts_arr;
    uint32_t* cps = nullptr;
    int64_t count = 0;
    bz_sb_content_cps(arr, &cps, &count);
    int8_t w = bz_cps_width(cps, count);
    size_t bytes = (size_t)count * (size_t)w;
    void* data = calloc(bytes + (size_t)w, 1);   // + width-sized NUL slot
    for (int64_t i = 0; i < count; i++) bz_put_cp(data, i, w, cps[i]);
    free(cps);
    *out_data = (char*)data;
    *out_width = w;
    return count;
}

// fix #7243 (spec §4.6 / stdlib §3.1, v0.344): the Object-vtable renderer.
//
// THE OPERATION IS ONE, SO THE ROUTES MUST AGREE. A builder's content reaches
// its consumers by two routes: the builder's OWN `ToString()` (stdlib §3.1's
// declared member), which the codegen lowers through `bz_strbuilder_flatten`
// (src/codegen/LlvmBackend_String.cpp's `emitStringBuilderToString`), and the
// `Object` vtable's ToString slot, which the runtime answers here. §8.5 ("a
// hole without a format spec formats via the value's ordinary `ToString()`")
// makes the interpolation hole and every `Object`-typed call the same
// operation, so the two routes must render the SAME text.
//
// WHAT WAS WRONG. The vtable route used to call the LEGACY BYTE-LEVEL
// `bz_strbuilder_to_string`, which concatenates the parts' RAW BYTES, and then
// `bz_make_string`, a WIDTH-1 constructor that takes no width — so the width
// was dropped on the floor and the result was read one byte per code point.
// "日" (U+65E5, stored as the two bytes E5 65 at width 2) came back as the
// TWO code points 229 and 101, while the direct route on the SAME builder
// returned the ONE code point 26085. The legacy byte-level view also lost the
// per-string width (§4.6) altogether: a byte blob is not a code-point
// sequence at all once a part is wider than one byte per code point.
//
// WHAT THIS DOES. It flattens the parts in the CODE-POINT domain and packs
// them at the NARROWEST width that fits the largest code point in the value —
// §4.6's own rule ("selected at construction based on the largest code point
// in the value"), and the rule the builder's own ToString already follows (see
// bz_sb_content_cps for why the width is observable: stdlib §9.6's `IsAscii`
// answers it in O(1)). The result is a fresh +1 `bz.string`, exactly like the
// direct route's.
//
// THE LEGACY HELPER IS GONE. `bz_strbuilder_to_string` had exactly ONE caller
// — this renderer — and its whole purpose was to be that caller's byte-level
// view of the content. With the caller repaired it has no caller left, and a
// byte-level view of the content is exactly the thing that must not come
// back: it is the defect. (`bz_sb_append_format` was the OTHER byte-level
// site — #7229's residual (b) — and it was repaired in the code-point domain
// by #7244's width half; it no longer consumes the rendered string's raw
// bytes either.)
extern "C" void* bz_strbuilder_to_string_obj(void* parts_arr) {
    auto* arr = (bz_array_t*)parts_arr;
    uint32_t* cps = nullptr;
    int64_t count = 0;
    bz_sb_content_cps(arr, &cps, &count);
    int8_t w = bz_cps_width(cps, count);
    void* s = bz_sb_string_from_cps(cps, count, w);
    free(cps);
    return s;
}

// ---------------------------------------------------------------------------
// StringBuilder mutators (§3.1). Strategy: flatten the parts to one code-point
// sequence, apply the operation, and reset the parts array to a single fresh
// +1 part. The builder OWNS each of its parts (Append retains / transfers a
// +1 per part, #1185), so before dropping the old parts each one is
// bz_string_release'd — otherwise the retained +1 would leak.
// bz_string_release is null-safe and a no-op on a sentinel/literal, so a
// null/never-stored slot is safe to pass.
//
// The mutators work in the CODE-POINT domain (spec §4.6 / stdlib §3.1,
// v0.344): the index and length a caller passes count CODE POINTS — §4.6 makes
// `s[i]` the char at code-point position i, and §3.1's bound is `0..Length`
// with `Length` a count of code points — while the content they address is
// the concatenation of the parts' code points. The previous implementation
// addressed the parts' concatenated BYTES, so an in-range index could land in
// the middle of a code point's storage (#7229).

// Replace the entire parts list with ONE freshly-allocated +1 string part
// holding `strObj` (which the builder then owns by transferring it in). Resets
// the array's count to 0 and adds the single new part.
static void bz_sb_reset_to(bz_array_t* arr, void* strObj) {
    // #1185: release each old owned part before dropping it (the builder owns a
    // +1 per part from Append/ctor). bz_string_release is null-/sentinel-safe.
    if (arr->data) {
        for (int64_t i = 0; i < arr->count; ++i) {
            void* part = ((void**)arr->data)[i];
            if (part) bz_string_release(part);
        }
    }
    arr->count = 0;                                  // drop old parts
    bz_array_add(arr, &strObj, 8);
}

// per §3.1 (StringBuilder.Insert(index, value)): insert `value`'s CODE POINTS
// at the code-point index `index` of the builder's content. Out-of-range
// (index < 0 or > Length) is a no-op here; the codegen-side bounds check
// raises IndexOutOfRangeException per the §3.1 v0.195 contract. The insert
// does NOT retain `value` — it COPIES `value`'s code points into the new flat
// buffer; the caller still owns the value. The builder resets to one part
// (via bz_sb_reset_to), releasing the old +1 parts first.
void bz_sb_insert(void* parts_arr, int64_t index, const void* value) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || !value) return;
    uint32_t* cps = nullptr;
    int64_t count = 0;
    bz_sb_content_cps(arr, &cps, &count);            // current content
    if (index < 0 || index > count) { free(cps); return; }  // no-op on OOB
    auto* v = (bz_string_struct*)value;
    int64_t vcount = v->len;
    int64_t total = count + vcount;
    uint32_t* out = (uint32_t*)malloc((size_t)(total > 0 ? total : 1) *
                                       sizeof(uint32_t));
    int64_t o = 0;
    for (int64_t i = 0; i < index; i++) out[o++] = cps[i];      // prefix
    for (int64_t i = 0; i < vcount; i++) out[o++] = bz_str_cp(v, i);  // value
    for (int64_t i = index; i < count; i++) out[o++] = cps[i];  // suffix
    free(cps);
    // The new part is stored at the NARROWEST width that fits the largest code
    // point in the NEW value (§4.6) — not at the widest of the two operands'
    // widths: the value's width is a property of the value, and a mutator may
    // narrow the content.
    void* strObj = bz_sb_string_from_cps(out, total, bz_cps_width(out, total));
    free(out);
    bz_sb_reset_to(arr, strObj);
}

// per §3.1 (StringBuilder.Remove(startIndex, length)): remove `length` CODE
// POINTS starting at the code-point index `start_index` of the builder's
// content. Per the §3.1 v0.195 contract, an out-of-range request raises
// IndexOutOfRangeException at the codegen layer; the runtime clamps an
// out-of-range window to a no-op (the SB is unchanged) so a stray caller
// cannot corrupt the parts list. The builder resets to one part, releasing
// the old +1 parts first.
void bz_sb_remove(void* parts_arr, int64_t start_index, int64_t length) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr) return;
    uint32_t* cps = nullptr;
    int64_t count = 0;
    bz_sb_content_cps(arr, &cps, &count);
    // §3.1 / §9.6 Remove: start and length must denote a valid range; an
    // out-of-range request is a no-op here (the SB is unchanged).
    if (length < 0 || start_index < 0 || start_index + length > count) {
        free(cps);
        return;
    }
    int64_t total = count - length;
    uint32_t* out = (uint32_t*)malloc((size_t)(total > 0 ? total : 1) *
                                       sizeof(uint32_t));
    int64_t o = 0;
    for (int64_t i = 0; i < start_index; i++) out[o++] = cps[i];   // prefix
    for (int64_t i = start_index + length; i < count; i++) out[o++] = cps[i];  // suffix
    free(cps);
    // The new part is stored at the NARROWEST width that fits the largest code
    // point in the NEW value (§4.6) — a removal may narrow the content, and
    // the width must follow the value (stdlib §9.6's IsAscii answers from the
    // width in O(1)).
    void* strObj = bz_sb_string_from_cps(out, total, bz_cps_width(out, total));
    free(out);
    bz_sb_reset_to(arr, strObj);
}

// per §3.1 (StringBuilder.Replace(oldValue, newValue)): replace every
// non-overlapping occurrence of `old_value`'s CODE-POINT sequence in the
// builder's content with `new_value`'s code points. The §3.1 v0.195 contract
// raises ArgumentException for an empty oldValue; the runtime short-circuits
// that case (no-op) — the rejection itself is #7228, not this fix. The
// builder resets to one part, releasing the old +1 parts first.
//
// The match is over CODE POINTS, not over bytes: a byte-level match could
// straddle a code point's storage (and a byte-level match of a width-1
// `old_value` against a wider content could hit the low byte of a wide code
// point), so the two operands are compared as code-point sequences whatever
// their widths (§4.6).
void bz_sb_replace(void* parts_arr, const void* old_value, const void* new_value) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || !old_value || !new_value) return;
    auto* ov = (bz_string_struct*)old_value;
    auto* nv = (bz_string_struct*)new_value;
    int64_t ocount = ov->len;
    if (ocount <= 0) return;                          // empty oldValue: no-op
    uint32_t* cps = nullptr;
    int64_t count = 0;
    bz_sb_content_cps(arr, &cps, &count);
    uint32_t* ocps = (uint32_t*)malloc((size_t)ocount * sizeof(uint32_t));
    for (int64_t i = 0; i < ocount; i++) ocps[i] = bz_str_cp(ov, i);
    int64_t ncount = nv->len;
    uint32_t* ncps = (uint32_t*)malloc((size_t)(ncount > 0 ? ncount : 1) *
                                        sizeof(uint32_t));
    for (int64_t i = 0; i < ncount; i++) ncps[i] = bz_str_cp(nv, i);
    // First pass: count occurrences to size the result exactly.
    int64_t occ = 0;
    for (int64_t i = 0; i + ocount <= count;) {
        if (memcmp(cps + i, ocps, (size_t)ocount * sizeof(uint32_t)) == 0) {
            occ++; i += ocount;
        } else i++;
    }
    if (occ == 0) {                                  // nothing to do
        free(cps); free(ocps); free(ncps);
        return;
    }
    int64_t total = count + occ * (ncount - ocount);
    uint32_t* out = (uint32_t*)malloc((size_t)(total > 0 ? total : 1) *
                                       sizeof(uint32_t));
    int64_t off = 0;
    for (int64_t i = 0; i < count;) {
        if (i + ocount <= count &&
            memcmp(cps + i, ocps, (size_t)ocount * sizeof(uint32_t)) == 0) {
            for (int64_t k = 0; k < ncount; k++) out[off++] = ncps[k];
            i += ocount;
        } else {
            out[off++] = cps[i++];
        }
    }
    free(cps); free(ocps); free(ncps);
    // The new part is stored at the NARROWEST width that fits the largest code
    // point in the NEW value (§4.6) — a replacement may narrow the content
    // ("日" -> "z"), and the width must follow the value (stdlib §9.6's
    // IsAscii answers from the width in O(1)).
    void* strObj = bz_sb_string_from_cps(out, total, bz_cps_width(out, total));
    free(out);
    bz_sb_reset_to(arr, strObj);
}

// stdlib §3.1 AppendFormat(format, args): substitute the .NET-style composite
// placeholders {N} in `format` with the string form of args[N] (resolved via
// virtual ToString, vtable slot 3 — the uniform per-type thunk a boxed Object
// carries), then append the result to the builder's parts list (exactly like
// Append, which adds a fresh +1 part). A literal brace is escaped as {{ }}.
// Alignment (`,W`) and a sub-format specifier (`:F`) after the index are skipped
// to reach the index, which is what the bare {N} form requires; a no-placeholder
// format contributes its literal text. This is the #1179 fix: AppendFormat
// previously had no codegen/runtime case and emitted nothing. `format` is a
// bz.string*; `args` is a bz_array_t* of object pointers (Array<Object>).
void bz_sb_append_format(void* parts_arr, const void* format_ptr,
                         const void* args_arr) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || !format_ptr) return;
    auto* fmt = (bz_string_struct*)format_ptr;
    auto* args = (bz_array_t*)args_arr;

    // fix #7244 (spec §4.6 / stdlib §3.1, v0.344): the format and every
    // rendered argument are CODE-POINT SEQUENCES, not byte strings.
    //
    // §4.6 (v0.344): a `string` "stores each code point in `width` bytes (1, 2
    // or 4), the width chosen PER STRING at construction based on the largest
    // code point in the value" — so a string's DATA is not its TEXT: a width-2
    // value's two bytes are ONE code point, and reading them as two bytes
    // yields two different characters.
    //
    // The previous implementation worked in the BYTE domain throughout: it
    // read the format as raw bytes (`nb = fmt->len * fmt->width`, i.e. the
    // format's BYTE count) and copied each rendered argument's raw bytes into
    // a width-1 output buffer. Two defects follow from that, and both are
    // observable:
    //
    //   * A WIDE ARGUMENT was split into Latin-1 code points. The rendered
    //     "日" (U+65E5, stored as the two bytes E5 65 at width 2) became the
    //     TWO code points 229 and 101, so the builder's Length counted two
    //     characters where the argument's text has one, and the builder's
    //     content was not the argument's text at all.
    //   * A WIDE FORMAT was read as 16-bit units interpreted as BYTES, so its
    //     literal text was garbage (every other byte was a NUL).
    //
    // The fix works in the code-point domain end to end: the format's code
    // points are read one at a time, each rendered argument contributes ITS
    // OWN code points, and the result is appended as a fresh part packed at
    // the narrowest width that fits its largest code point (§4.6's own rule —
    // the same rule `bz_sb_content_cps` and `bz_cps_width` already apply to
    // the builder's other mutators).
    //
    // Read one code point of `s` at code-point index `i`. A width outside
    // {1,2,4} is malformed; answer as width 1 (the pre-fix byte-mode
    // assumption) rather than reading the buffer at a width it does not have.
    auto scp = [](const bz_string_struct* s, int64_t i) -> uint32_t {
        if (s->width == 2) return ((const uint16_t*)s->data)[i];
        if (s->width == 4) return ((const uint32_t*)s->data)[i];
        return ((const uint8_t*)s->data)[i];
    };
    // The format's code-point count (§4.6's length) and its code points.
    const int64_t nf = fmt->len;
    auto fcp = [&](int64_t i) -> uint32_t { return scp(fmt, i); };
    int64_t nargs = args ? args->count : 0;
    void** argSlots = args ? (void**)args->data : nullptr;

    // Resolve every arg's string form ONCE (virtual ToString, vtable slot 3) so
    // the two passes below share one cached {data,len} per arg — ToString is not
    // re-invoked (it allocates; calling it twice would double the tolerated leak
    // and could observe side effects twice). A null/undispatchable arg yields "".
    //
    // fix #7272 (spec §28.9, v0.344; the slot-3 contract in
    // src/codegen/LlvmBackend_String.cpp's `lowerObjectToString`): the slot-3
    // renderer hands back a reference the CALLER owns — the same contract
    // `lowerObjectToString` states and relies on ("the vtable thunks mint, a
    // literal is an immortal sentinel on which retain/release are no-ops"), so
    // the value is EITHER a fresh +1 heap string or an immortal sentinel. This
    // helper is the caller: it borrows the rendered CODE POINTS and copies them
    // into its own buffer, so the reference is ITS to discharge. It did not —
    // the rendered string leaked (measured: 34 B / 2 allocations for a
    // `StringBuilder` argument, whose render is `bz_strbuilder_to_string_obj`'s
    // fresh +1). The release is DEFERRED past the second pass below, because
    // the second pass reads out of the rendered string's OWN data buffer,
    // which the release frees.
    //
    // NOTE ON THE SPEC ANCHOR. §28.3 K1.6 (v0.344) says the runtime-managed
    // types — `string` included — "carry no classification and no obligation",
    // and §28.9 (v0.344) says of them that "how the runtime manages their
    // lifecycle (reference counting, region allocation, or other) is not part
    // of this specification". So the SPEC's ownership kernel imposes NO
    // disposal obligation on the rendered string: the release below rests on
    // the slot-3 contract and on the renderers' own minting behaviour, not on
    // K1.1. (K1.1's "owned — the holder carries the disposal obligation"
    // applies to a CLASSIFIED value, and a `string` is not one.)
    //
    // `bz_string_release` is null-safe and a no-op on the immortal sentinel, so
    // a uniform release over every rendered argument is sound — the same
    // reasoning `bz_sb_reset_to` (below) applies to the builder's own parts.
    //
    // THE STRUCT IS DELIBERATELY 16 BYTES — the SAME SIZE the pre-fix
    // `{const char* data; int64_t len;}` was. The rendered string's DATA
    // pointer is DERIVED from the owned string object rather than cached
    // beside it, so this fix adds no field and grows no allocation. That
    // matters: #7244's `string`-argument SEGFAULT reads the argument's DATA
    // pointer as a vtable and then reads slot 3 out of the DATA BUFFER, so
    // whether it faults depends on the bytes that follow the buffer — i.e. on
    // the heap layout. Changing this allocation's size would silently change
    // #7244's reproduction.
    //
    // #7244 HAS TWO HALVES. The WIDTH half — this function's byte-level
    // rendering of the format and of each rendered argument — is repaired
    // here (see the code-point-domain comment above). The VTABLE half — a
    // `string` argument has no vtable, so the slot-3 dispatch below reads the
    // string's DATA pointer as one — is NOT repairable here: the sound
    // discriminator is `bz_obj_is_string(obj, vtable_table)`
    // (src/runtime/bezel_runtime.h), and the vtable table is the per-module
    // global the CODEGEN emits, so only the AppendFormat lowering in
    // src/codegen/LlvmBackend_Calls.cpp can supply it. That half stays OPEN.
    struct ArgStr { void* owned; int64_t len; };
    ArgStr* astr = (ArgStr*)malloc((size_t)(nargs > 0 ? nargs : 1) * sizeof(ArgStr));
    for (int64_t k = 0; k < nargs; k++) {
        astr[k].owned = nullptr;
        astr[k].len = 0;
        void* obj = argSlots ? argSlots[k] : nullptr;
        if (!obj) continue;
        void* vtable; memcpy(&vtable, obj, sizeof(void*));
        if (!vtable) continue;
        void* toStrFnP; memcpy(&toStrFnP,
            (uint8_t*)vtable + 3 * sizeof(void*), sizeof(void*));
        if (!toStrFnP) continue;
        typedef void* (*bz_tostr_fn)(void*);
        void* strObj = ((bz_tostr_fn)toStrFnP)(obj);
        if (!strObj) continue;
        auto* s = (bz_string_struct*)strObj;
        astr[k].len = s->len;                        // a CODE-POINT count (§4.6)
        astr[k].owned = strObj;                    // the +1 this helper owns
    }
    // The rendered CODE POINTS live in the owned string's own data buffer (at
    // that string's own width); a null/undispatchable argument has no owned
    // string and renders as "".
    auto argCps = [&](int64_t k, int64_t i) -> uint32_t {
        return scp((const bz_string_struct*)astr[k].owned, i);
    };

    // Parse one placeholder beginning at fcp(i)=='{'. On success sets idx/close
    // (index of '}') and returns true; a malformed/empty index returns false.
    auto parsePlaceholder = [&](int64_t i, int64_t& idx, int64_t& close)
        -> bool {
        int64_t j = i + 1;
        if (j >= nf) return false;                               // need a digit
        uint32_t d = fcp(j);
        if (d < '0' || d > '9') return false;                    // need a digit
        int64_t v = 0;
        while (j < nf) {
            uint32_t c = fcp(j);
            if (c < '0' || c > '9') break;
            v = v * 10 + (int64_t)(c - '0');
            j++;
        }
        // Skip optional ,alignment and :formatString up to the closing brace.
        while (j < nf && fcp(j) != '}') j++;
        if (j >= nf) return false;                               // no closing brace
        idx = v;
        close = j;
        return true;
    };

    // First pass: the output's CODE-POINT count (§4.6's length).
    int64_t total = 0;
    for (int64_t i = 0; i < nf;) {
        uint32_t c = fcp(i);
        if (c == '{') {
            if (i + 1 < nf && fcp(i + 1) == '{') { total++; i += 2; continue; }
            int64_t idx = -1, close = -1;
            if (parsePlaceholder(i, idx, close)) {
                if (idx >= 0 && idx < nargs) total += astr[idx].len;
                i = close + 1;
            } else { total++; i++; }                              // literal '{'
        } else if (c == '}') {
            if (i + 1 < nf && fcp(i + 1) == '}') { total++; i += 2; continue; }
            total++; i++;
        } else { total++; i++; }
    }

    // Second pass: fill the CODE-POINT buffer.
    uint32_t* cps = (uint32_t*)malloc((size_t)(total > 0 ? total : 1) *
                                       sizeof(uint32_t));
    int64_t off = 0;
    for (int64_t i = 0; i < nf;) {
        uint32_t c = fcp(i);
        if (c == '{') {
            if (i + 1 < nf && fcp(i + 1) == '{') { cps[off++] = '{'; i += 2; continue; }
            int64_t idx = -1, close = -1;
            if (parsePlaceholder(i, idx, close)) {
                if (idx >= 0 && idx < nargs && astr[idx].len > 0) {
                    for (int64_t k = 0; k < astr[idx].len; k++)
                        cps[off++] = argCps(idx, k);
                }
                i = close + 1;
            } else { cps[off++] = c; i++; }
        } else if (c == '}') {
            if (i + 1 < nf && fcp(i + 1) == '}') { cps[off++] = '}'; i += 2; continue; }
            cps[off++] = c; i++;
        } else { cps[off++] = c; i++; }
    }

    // fix #7272 (spec §28.9, v0.344; the slot-3 contract in
    // src/codegen/LlvmBackend_String.cpp's `lowerObjectToString`): discharge
    // the reference the slot-3 renderer handed us. The CODE POINTS are copied
    // (the second pass above read them out of the rendered string's own data
    // buffer), so this helper holds the last reference to it and nobody else
    // will. The release is here — AFTER the second pass — because the second
    // pass reads out of the rendered string's own data buffer, which this
    // frees.
    //
    // `bz_string_release` is null-safe and a no-op on the immortal literal
    // sentinel (rc == BZ_STRING_RC_SENTINEL), so a uniform release over every
    // rendered argument is sound: the renderers return EITHER a fresh +1 heap
    // string (the default class/enum/record render, `bz_sb_obj_tostring`,
    // `bz_array_obj_tostring`'s fallback, `bz_mutex_obj_tostring`,
    // `bz_char_to_string`, `emitPrimitiveToString`) OR an immortal sentinel
    // (the §8.5 default type name, the array's stamped descriptor name). This
    // is the same reasoning `bz_sb_reset_to` applies to the builder's parts.
    for (int64_t k = 0; k < nargs; k++)
        if (astr[k].owned) bz_string_release(astr[k].owned);
    free(astr);

    // Wrap the formatted CODE POINTS as a fresh +1 part and append it (like
    // Append — the builder does not own/retain its parts, so the allocation is
    // the existing tolerated leak; no part is freed here).
    //
    // fix #7244 (spec §4.6, v0.344): the part is packed at the NARROWEST
    // width that fits its largest code point — §4.6's own rule ("selected at
    // construction based on the largest code point in the value"). The
    // pre-fix part was a WIDTH-1 buffer whose `len` was a BYTE count, so a
    // wide result was not a string at all: its bytes were read back as
    // Latin-1 code points. `bz_sb_string_from_cps` is the same constructor
    // the builder's other mutators use, so the part's width now follows its
    // content exactly as they do.
    void* strObj = bz_sb_string_from_cps(cps, off, bz_cps_width(cps, off));
    free(cps);
    bz_array_add(arr, &strObj, 8);
}

// per §32.5/§32.6: re-encode a bz_string's code points to a fresh,
// null-terminated UTF-8 buffer for the CString FFI boundary.
//
// The internal storage is raw code points at the string's width (1/2/4 bytes
// each); for width>1 those bytes contain interior zeros that truncate a C
// `char*`. Width 1 holds Latin-1 code points (0x00..0xFF) — bytes 0x80..0xFF
// are NOT valid UTF-8 either, so we re-encode every width uniformly rather than
// only width>1, which keeps the conversion correct for all inputs (width-1
// pure-ASCII strings still produce the identical bytes). The caller (the
// width-1 marshalling fast path in codegen) may still pass the raw data
// pointer for pure-ASCII strings; this helper handles the general case.
extern "C" char* bz_string_to_utf8z(const void* strp) {
    if (!strp) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    // Worst case: every code point is U+10FFFF → 4 UTF-8 bytes; +1 for NUL.
    size_t cap = (size_t)n * 4 + 1;
    char* out = (char*)malloc(cap);
    if (!out) return nullptr;
    size_t off = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        // UTF-8 encode one code point.
        if (cp <= 0x7F) {
            out[off++] = (char)cp;
        } else if (cp <= 0x7FF) {
            out[off++] = (char)(0xC0 | (cp >> 6));
            out[off++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out[off++] = (char)(0xE0 | (cp >> 12));
            out[off++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[off++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[off++] = (char)(0xF0 | (cp >> 18));
            out[off++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[off++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[off++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[off] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// stdlib §10.1 / §32.6 / fix #977: CString.FromBytes(bytes: Array<byte>)
// Copies the byte-array's contents into a fresh malloc'd buffer and
// appends a null terminator. The caller is responsible for the bytes being
// valid (null-terminated, valid UTF-8 per §10.1). The returned char* is
// owned by the caller and must be released via free().
extern "C" char* bz_cstring_from_bytes(const void* arrp) {
    if (!arrp) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    const auto* arr = (const bz_array_t*)arrp;
    int64_t n = arr->count;
    if (n < 0) n = 0;
    // elem_size must be 1 (Array<byte>); copy n bytes + NUL.
    char* out = (char*)malloc((size_t)n + 1);
    if (!out) return nullptr;
    if (n > 0 && arr->data) memcpy(out, arr->data, (size_t)n);
    out[n] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// stdlib §10.1 / issue #1000: CString.ToBytes(): owned Array<byte>
// Returns a FRESH, independently-owned Array<byte> (bz_array_t) holding a COPY
// of the CString's bytes. The count is the byte length EXCLUDING the null
// terminator (the same value §10.1 `Length` reports via strlen); elem_size is 1.
//
// The result must NOT alias the CString's own buffer: the round-trip
//   using var s := CString.FromBytes(b); using var back := s.ToBytes();
// disposes BOTH `s` (free(char*)) and `back` (bz_array_free) at scope end. If the
// returned array's `data` aliased the CString buffer, free() and bz_array_free's
// free(arr->data) would release the same allocation twice (free(): invalid
// pointer / SIGABRT). A fresh malloc + memcpy gives `back` its own buffer, so
// each owner frees exactly once (§28.3 dispose-exactly-once).
extern "C" void* bz_cstring_to_bytes(const char* cstr) {
    auto* arr = (bz_array_t*)calloc(1, sizeof(bz_array_t));
    if (!arr) return nullptr;
    arr->vtable = (void*)bz_array_vtable;  // fix #6776 (§8.2): identity at allocation
    arr->kind = BZ_RUNTIME_KIND_ARRAY;  // fix #3026
    if (!cstr) {
        // Null CString → empty owned Array<byte> (count 0, no data buffer).
        return arr;
    }
    size_t n = strlen(cstr);  // byte length excluding the null terminator (§10.1)
    if (n > 0) {
        arr->data = malloc(n);
        if (!arr->data) { free(arr); return nullptr; }
        memcpy(arr->data, cstr, n);
        arr->capacity = (int64_t)n;
        arr->count    = (int64_t)n;
    }
    return arr;
}

// ---------------------------------------------------------------------------
// stdlib §10.2 / §32.6 / fix #977: WString.From(s: string)
// Produces a fresh malloc'd null-terminated UTF-16LE wchar_t* buffer from a
// Bezel string. Each Bezel code point is encoded as one UTF-16 code unit for
// BMP points (U+0000..U+D7FF, U+E000..U+FFFF) or as a surrogate pair for
// code points above U+FFFF. The trailing null is two zero bytes (one wchar_t).
// On platforms where wchar_t is 4 bytes (Linux) we use uint16_t for the
// individual code units and cast, keeping a portable 2-byte-per-code-unit
// encoding. The returned pointer is owned by the caller (free()).
extern "C" void* bz_string_to_utf16z(const void* strp) {
    if (!strp) {
        // Empty WString: two zero bytes (one null wchar_t).
        uint16_t* empty = (uint16_t*)malloc(sizeof(uint16_t));
        if (empty) empty[0] = 0;
        return empty;
    }
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    // Worst case: every code point above U+FFFF needs a surrogate pair (2 units).
    size_t cap = ((size_t)n * 2 + 1) * sizeof(uint16_t);
    uint16_t* out = (uint16_t*)malloc(cap);
    if (!out) return nullptr;
    size_t off = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        if (cp < 0x10000) {
            out[off++] = (uint16_t)cp;
        } else {
            // Surrogate pair per UTF-16.
            cp -= 0x10000;
            out[off++] = (uint16_t)(0xD800 | (cp >> 10));
            out[off++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        }
    }
    out[off] = 0;
    return out;
}

// stdlib §10.2 / fix #983: WString.Length — the number of UTF-16 code units in a
// null-terminated uint16_t buffer, excluding the terminating zero unit. (strlen
// cannot be used: UTF-16 of an ASCII code point has a high zero byte, so a
// byte-wise scan would stop early.)
extern "C" int64_t bz_wstrlen(const void* wstrp) {
    if (!wstrp) return 0;
    const uint16_t* p = (const uint16_t*)wstrp;
    int64_t n = 0;
    while (p[n] != 0) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// stdlib §10.3 / fix #985: RawBytes is a pointer-and-length pair. The runtime
// representation is a small heap struct { void* ptr; int64_t len } so that
// .Length, .Pointer, and .CopyTo are backed (issue #985: the prior rep was a
// bare byte buffer that dropped the length). The byte buffer (`ptr`) is an
// independently-owned malloc'd copy of the source array's bytes; `len` is the
// source array's count. Disposal frees BOTH the buffer and the struct exactly
// once via bz_rawbytes_free (the `ffiBuffer` disposal path is split so that
// CString/WString stay a single free of their bare buffer, but RawBytes frees
// its two-level allocation — see LlvmBackend_Setup.cpp).
struct bz_rawbytes_t {
    void*   ptr;   // owned byte buffer (malloc); null only for a 0-length empty
    int64_t len;   // byte count
};

// stdlib §10.3 / fix #977 / #985: RawBytes.From(buffer: Array<byte>) — copy the
// byte array's contents into a fresh independently-owned malloc'd buffer and
// wrap it with its length in a heap struct. Unlike CString.FromBytes there is
// no null terminator — the length is carried explicitly in the struct's `len`
// field. The returned struct (and its buffer) are owned by the caller, released
// via bz_rawbytes_free. Mutations to the source array after construction do not
// affect the RawBytes (§10.3): the bytes are copied here.
extern "C" void* bz_rawbytes_from(const void* arrp) {
    auto* rb = (bz_rawbytes_t*)malloc(sizeof(bz_rawbytes_t));
    if (!rb) return nullptr;
    rb->ptr = nullptr;
    rb->len = 0;
    if (!arrp) return rb;          // empty RawBytes (len 0, no buffer)
    const auto* arr = (const bz_array_t*)arrp;
    int64_t n = arr->count;
    if (n <= 0) return rb;         // empty: len 0, ptr null (no malloc(0))
    void* out = malloc((size_t)n);
    if (!out) { rb->len = n; return rb; }  // OOM: report length, null buffer
    if (arr->data) memcpy(out, arr->data, (size_t)n);
    rb->ptr = out;
    rb->len = n;
    return rb;
}

// stdlib §10.3 / fix #985: RawBytes.Length — the byte count carried in the
// struct. Null-guarded (a disposed/zeroed slot reads as length 0).
extern "C" int64_t bz_rawbytes_length(const void* rbp) {
    if (!rbp) return 0;
    return ((const bz_rawbytes_t*)rbp)->len;
}

// stdlib §10.3 / fix #985: RawBytes.Pointer — the underlying byte buffer
// address as a raw pointer (codegen wraps it as an IntPtr). Valid until the
// RawBytes is disposed (§10.3). Null-guarded.
extern "C" void* bz_rawbytes_ptr(const void* rbp) {
    if (!rbp) return nullptr;
    return ((const bz_rawbytes_t*)rbp)->ptr;
}

// stdlib §10.3 / fix #985: RawBytes.CopyTo(destination: Array<byte>, destOffset)
// — copy `len` bytes from the RawBytes buffer into `destination` starting at
// `destOffset`. Bounds are checked against the destination's count; an
// out-of-range copy is clamped to the available space (no overrun). A null or
// empty RawBytes copies nothing.
extern "C" void bz_rawbytes_copyto(const void* rbp, void* destp,
                                   int64_t destOffset) {
    if (!rbp || !destp) return;
    const auto* rb = (const bz_rawbytes_t*)rbp;
    if (rb->len <= 0 || !rb->ptr) return;
    auto* dest = (bz_array_t*)destp;
    if (!dest->data || destOffset < 0) return;
    int64_t avail = dest->count - destOffset;
    if (avail <= 0) return;
    int64_t n = rb->len < avail ? rb->len : avail;
    memcpy((char*)dest->data + destOffset, rb->ptr, (size_t)n);
}

// stdlib §10.3 / fix #985: dispose a RawBytes — free the owned byte buffer,
// then the struct itself. Exactly-once: the disposal path null-guards the slot
// before calling this, and idempotent on a null argument. Distinct from the
// single free() used for CString/WString (which are bare buffers).
extern "C" void bz_rawbytes_free(void* rbp) {
    if (!rbp) return;
    auto* rb = (bz_rawbytes_t*)rbp;
    free(rb->ptr);
    free(rb);
}

// ---------------------------------------------------------------------------
// stdlib §2.18 (v0.345) / fix #7334: the FLOATING row of the format-specifier
// table, rendered from a format specifier that is only known at RUN time.
//
// §2.18's Floating row reads: "`F`/`Fn` fixed-point (n decimals, default 2) ·
// `E`/`En` scientific · `G`/`Gn` general · `P`/`Pn` percent (×100, `%` suffix)
// · `R` round-trip" — the document fixes a DEFAULT DIGIT COUNT FOR `F` ONLY,
// and says NOTHING about a default for `E`, `G` or `P`. The defaults used below
// for `E`/`G`/`P` are the HOUSE's, not the document's: they are the SHARED
// routine's — `emitWithFormatSpec` (src/codegen/LlvmBackend_String.cpp) renders
// "%.6E"/"%.6G" and "%.2f"/"%.2f%%" — so that the two spellings of one
// specifier answer alike.
//
// The row's fault (stdlib §2.18, v0.345): "An unrecognized specifier raises
// `FormatException` (strict-fault, as everywhere)".
//
// The LITERAL spelling of `ToString(format)` renders this table in
// `emitWithFormatSpec` (src/codegen/LlvmBackend_String.cpp); the helpers
// below are the RUN-time spelling's half of the SAME table, so the two
// spellings of one specifier answer alike. Each rule below mirrors the
// shared routine's, one by one:
//
//   * the letter is case-INSENSITIVE, and its CASE fixes the output case for
//     `E`/`G` ("e" → "1.500000e+00", "E" → "1.500000E+00"); `F` and `P` have
//     no case-sensitive output;
//   * the digit count after the letter is optional; `F`/`P` default to two
//     decimals and `E`/`G` to six — the HOUSE's defaults (§2.18 fixes only
//     `F`'s);
//   * the count is CLAMPED to 65 — the same bound the literal spelling clamps
//     it to (fix #7286), so a huge count renders alike at both spellings;
//   * `R` is the round-trip rendering and takes NO count ("R2" is not in the
//     vocabulary);
//   * `P` multiplies the value by 100 and appends a literal '%';
//   * a NON-FINITE value renders as "NaN" / "Infinity" / "-Infinity" under
//     EVERY floating specifier (stdlib §2.18, v0.344, #7301) — the names of
//     the §9.2 constants, with no digits, no exponent and no '%' suffix;
//   * an EMPTY format is the NO-FORMAT rendering (the plain ToString()), which
//     is what the literal spelling of `ToString("")` renders.
//
// `fmt_p` is a bz_string {data,len,width,rc} — the lowered format argument.
// Returns nullptr for a specifier outside the vocabulary; the codegen caller
// then raises FormatException (§2.18's strict fault).
// ---------------------------------------------------------------------------

// The code point at index `i` of a bz_string: the internal storage holds code
// points at the string's own width (1/2/4 bytes each — stdlib §3.1).
static inline uint32_t bz_fmt_code_point(const bz_string_hdr* fh, int64_t i) {
    if (fh->width == 1) return ((const uint8_t*)fh->data)[i];
    if (fh->width == 2) return ((const uint16_t*)fh->data)[i];
    return ((const uint32_t*)fh->data)[i];
}

// §2.18's floating vocabulary, decoded from a format string.
struct bz_float_spec {
    char kind;      // 'F', 'E', 'G', 'P' or 'R'
    char conv;      // the output conversion: 'f', 'E'/'e', 'G'/'g'
    int  count;     // the digit count (clamped to 65)
    bool hasCount;  // a digit count was written
};

// Decode `fh` into §2.18's FLOATING row. Returns false when the specifier is
// outside that row (the caller raises FormatException).
static bool bz_decode_float_spec(const bz_string_hdr* fh, bz_float_spec* out) {
    const int64_t n = fh->len;
    if (n < 1 || !fh->data) return false;
    const uint32_t c0 = bz_fmt_code_point(fh, 0);
    if (c0 > 0x7F) return false;  // a non-ASCII code point is not a specifier
    const char c = (char)c0;
    const char up = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    if (up != 'F' && up != 'E' && up != 'G' && up != 'P' && up != 'R')
        return false;
    // The count after the letter is optional; a non-digit tail is not a count
    // but an unrecognized specifier (§2.18's strict fault).
    int count = 0;
    bool hasCount = false;
    for (int64_t i = 1; i < n; ++i) {
        const uint32_t d = bz_fmt_code_point(fh, i);
        if (d > 0x7F || d < '0' || d > '9') return false;
        hasCount = true;
        // The clamp: the LITERAL spelling rewrites a digit run whose value
        // exceeds 65 to "65" (fix #7286), so the two spellings agree.
        if (count <= 65) {
            count = count * 10 + (int)(d - '0');
            if (count > 65) count = 65;
        }
    }
    out->kind = up;
    out->count = count;
    out->hasCount = hasCount;
    switch (up) {
        case 'F':
            out->conv = 'f';
            break;
        case 'E':
            out->conv = (c == 'e') ? 'e' : 'E';
            break;
        case 'G':
            out->conv = (c == 'g') ? 'g' : 'G';
            break;
        case 'P':
            out->conv = 'f';  // percent: ×100 and a '%' suffix
            break;
        default:  // 'R' — round-trip
            // "R" takes NO count: "R2" is not in §2.18's floating vocabulary.
            if (hasCount) return false;
            out->conv = 'r';
            break;
    }
    return true;
}

// Render `v` with the decoded §2.18 floating specifier.
static void* bz_render_float_spec(double v, bool isFloat,
                                  const bz_float_spec* sp) {
    if (sp->kind == 'R') {
        // "R" (round-trip): the SHORTEST rendering that reads back as the same
        // value — the same text the no-format ToString() produces.
        return isFloat ? bz_float_to_string_r((float)v) : bz_double_to_string_r(v);
    }
    // per stdlib §2.18 (v0.344, #7301): a NON-FINITE value renders as the
    // NAME of its §9.2 constant under EVERY floating specifier — "NaN",
    // "Infinity", "-Infinity", with no digits, no exponent and no '%' suffix.
    // C's own "%f" prints "inf"/"nan", so a non-finite operand must not reach
    // snprintf.
    //
    // The test is on the OPERAND, before the percent scale, exactly as the
    // literal spelling does it: §2.18 speaks of the VALUE the caller holds, so
    // a finite operand keeps the rendering it had — including the case where
    // the ×100 of the percent specifier overflows to an infinity.
    if (std::isnan(v) || std::isinf(v)) return bz_double_to_string_r(v);
    const int prec = sp->hasCount
        ? sp->count
        : ((sp->kind == 'F' || sp->kind == 'P') ? 2 : 6);
    const char* fmt;
    switch (sp->kind) {
        case 'F': fmt = "%.*f"; break;
        case 'E': fmt = (sp->conv == 'e') ? "%.*e" : "%.*E"; break;
        case 'G': fmt = (sp->conv == 'g') ? "%.*g" : "%.*G"; break;
        default:  fmt = "%.*f%%"; break;  // 'P' — the percent specifier
    }
    const double val = (sp->kind == 'P') ? v * 100.0 : v;
    const int n = snprintf(nullptr, 0, fmt, prec, val);
    if (n < 0) return nullptr;
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) return nullptr;
    snprintf(buf, (size_t)n + 1, fmt, prec, val);
    void* s = bz_make_string(buf, n);
    free(buf);
    return s;
}

// stdlib §2.18 (v0.345) / fix #7334: `double.ToString(format)` — the format
// overload with a format specifier known only at RUN time.
extern "C" void* bz_double_to_string_fmt(double v, const void* fmt_p) {
    const bz_string_hdr* fh = (const bz_string_hdr*)fmt_p;
    if (!fh) return nullptr;  // no format at all → FormatException
    // An EMPTY format names no specifier, so it is the no-format rendering —
    // what the literal spelling of `ToString("")` renders.
    if (fh->len < 1) return bz_double_to_string_r(v);
    bz_float_spec sp;
    if (!bz_decode_float_spec(fh, &sp)) return nullptr;
    return bz_render_float_spec(v, /*isFloat=*/false, &sp);
}

// stdlib §2.18 (v0.345) / fix #7334: `float.ToString(format)` — the format
// overload with a format specifier known only at RUN time.
extern "C" void* bz_float_to_string_fmt(float v, const void* fmt_p) {
    const bz_string_hdr* fh = (const bz_string_hdr*)fmt_p;
    if (!fh) return nullptr;  // no format at all → FormatException
    if (fh->len < 1) return bz_float_to_string_r(v);
    bz_float_spec sp;
    if (!bz_decode_float_spec(fh, &sp)) return nullptr;
    // The float is widened to double for the fixed/scientific/general
    // renderings — exactly as the literal spelling does (an `FPExt`).
    return bz_render_float_spec((double)v, /*isFloat=*/true, &sp);
}
