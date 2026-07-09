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

// stdlib §9.6: IsAscii — 1 iff every code point < 128. A width≥2 string holds
// at least one code point ≥ 256, so it is never ASCII (the O(1) case); a width-1
// string is Latin-1 (code points ≤ 255), so a byte in [128,255] still defeats
// ASCII — scan for one. (#741)
int32_t bz_string_is_ascii(const void* sp) {
    const bz_string_struct* s = (const bz_string_struct*)sp;
    if (!s || s->len == 0) return 1;            // empty string is ASCII
    if (s->width != 1) return 0;                // ≥2-byte code points ⇒ not ASCII
    const uint8_t* d = (const uint8_t*)s->data;
    for (int64_t i = 0; i < s->len; ++i)
        if (d[i] >= 128) return 0;
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

// §9.6 / #1295: PadLeft/PadRight — build a fresh +1 bz_string of `totalWidth`
// code points by padding `self` with `padChar` (a code point). `leftAlign`
// selects PadRight (self at the front, pad after) vs PadLeft (pad first, self
// at the end). Strings store FIXED-WIDTH code points (width 1/2/4 bytes/cp,
// §4.6); the result width is the widest of self's width and the pad char's
// width, and self's code points are widened into it. The previous inline
// codegen assumed 1 byte per code point (memcpy'd the code-point COUNT as a
// byte count and hard-stored width=1), corrupting any code point > U+00FF.
extern "C" void* bz_str_pad(const void* self_p, int64_t totalWidth,
                            int32_t padChar, int8_t leftAlign) {
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
    if (leftAlign) {
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

// per §9.6 / #535: Substring(start, count): owned string — code-point slice.
// Returns a fresh +1 string of `count` code points starting at code-point
// index `start`. Clamps `count` to the available tail if needed.
// Preserves the source string's width (§4.6 per-string storage width).
// #1282 (§9.5 / §4.6): char.ToString() / interpolation of a char. A Bezel char
// is a Unicode scalar (up to U+10FFFF); build a single-code-point string at the
// narrowest storage width (1/2/4 bytes per §4.6 adaptive storage) so the full
// code point survives — the old codegen truncated it to its low byte (`%c`),
// corrupting any char above U+00FF. On output the string is UTF-8-encoded by
// bz_string_to_utf8z, so `char(0x0141)` renders as its UTF-8 form.
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

void bz_strbuilder_to_string(void* parts_arr, char** out_data, int64_t* out_len) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || arr->count == 0) {
        *out_data = (char*)calloc(1, 1);
        *out_len = 0;
        return;
    }
    // Compute total byte length (width 1 for all parts; we treat as bytes)
    int64_t total = 0;
    for (int64_t i = 0; i < arr->count; i++) {
        void* sptr = ((void**)arr->data)[i];
        if (!sptr) continue;
        auto* s = (bz_string_struct*)sptr;
        total += s->len * s->width;
    }
    char* buf = (char*)malloc((size_t)(total + 1));
    int64_t off = 0;
    for (int64_t i = 0; i < arr->count; i++) {
        void* sptr = ((void**)arr->data)[i];
        if (!sptr) continue;
        auto* s = (bz_string_struct*)sptr;
        int64_t bytes = s->len * s->width;
        memcpy(buf + off, s->data, (size_t)bytes);
        off += bytes;
    }
    buf[off] = '\0';
    *out_data = buf;
    *out_len = off;
}

// ---------------------------------------------------------------------------
// StringBuilder mutators (§3.1). Strategy: flatten the parts to one byte buffer,
// apply the operation, and reset the parts array to a single fresh +1 part. The
// builder does not own its parts (Append/Clear never release them), so resetting
// the count and overwriting slot 0 cannot double-free; old part headers are left
// as the existing tolerated leak. Width-1 (byte) mode, matching Append/ToString.

// Replace the entire parts list with one freshly-allocated +1 string holding
// `data[0..len)` (which `bz_sb_set_*` then owns by transferring it in). Resets
// the array's count to 0 and adds the single new part.
static void bz_sb_reset_to(bz_array_t* arr, char* data, int64_t len) {
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &data, sizeof(void*));
    memcpy(strObj + 8, &len, sizeof(int64_t));
    strObj[16] = 1;                                  // width = 1 (byte mode)
    bz_string_init_rc(strObj);                       // #422: producer ref, rc := 1
    arr->count = 0;                                  // drop old parts (not freed)
    bz_array_add(arr, &strObj, 8);
}

void bz_sb_insert(void* parts_arr, int64_t index, const void* value) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || !value) return;
    char* flat; int64_t flen;
    bz_strbuilder_to_string(parts_arr, &flat, &flen);  // current content
    if (index < 0 || index > flen) { free(flat); return; }  // no-op on OOB
    auto* v = (bz_string_struct*)value;
    int64_t vbytes = v->len * v->width;
    int64_t total = flen + vbytes;
    char* buf = (char*)malloc((size_t)(total + 1));
    memcpy(buf, flat, (size_t)index);                  // prefix
    memcpy(buf + index, v->data, (size_t)vbytes);      // inserted value
    memcpy(buf + index + vbytes, flat + index,
           (size_t)(flen - index));                    // suffix
    buf[total] = '\0';
    free(flat);
    bz_sb_reset_to(arr, buf, total);
}

void bz_sb_remove(void* parts_arr, int64_t start_index, int64_t length) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr) return;
    char* flat; int64_t flen;
    bz_strbuilder_to_string(parts_arr, &flat, &flen);
    // §3.1 / §9.6 Remove: start and length must denote a valid range; an
    // out-of-range request is a no-op here (the SB is unchanged).
    if (length < 0 || start_index < 0 || start_index + length > flen) {
        free(flat);
        return;
    }
    int64_t total = flen - length;
    char* buf = (char*)malloc((size_t)(total + 1));
    memcpy(buf, flat, (size_t)start_index);            // prefix
    memcpy(buf + start_index, flat + start_index + length,
           (size_t)(flen - start_index - length));     // suffix
    buf[total] = '\0';
    free(flat);
    bz_sb_reset_to(arr, buf, total);
}

void bz_sb_replace(void* parts_arr, const void* old_value, const void* new_value) {
    auto* arr = (bz_array_t*)parts_arr;
    if (!arr || !old_value || !new_value) return;
    auto* ov = (bz_string_struct*)old_value;
    auto* nv = (bz_string_struct*)new_value;
    int64_t obytes = ov->len * ov->width;
    if (obytes <= 0) return;                            // empty oldValue: no-op
    char* flat; int64_t flen;
    bz_strbuilder_to_string(parts_arr, &flat, &flen);
    int64_t nbytes = nv->len * nv->width;
    // First pass: count occurrences to size the result exactly.
    int64_t occ = 0;
    for (int64_t i = 0; i + obytes <= flen;) {
        if (memcmp(flat + i, ov->data, (size_t)obytes) == 0) { occ++; i += obytes; }
        else i++;
    }
    if (occ == 0) { free(flat); return; }              // nothing to do
    int64_t total = flen + occ * (nbytes - obytes);
    char* buf = (char*)malloc((size_t)(total + 1));
    int64_t off = 0;
    for (int64_t i = 0; i < flen;) {
        if (i + obytes <= flen &&
            memcmp(flat + i, ov->data, (size_t)obytes) == 0) {
            memcpy(buf + off, nv->data, (size_t)nbytes);
            off += nbytes;
            i += obytes;
        } else {
            buf[off++] = flat[i++];
        }
    }
    buf[total] = '\0';
    free(flat);
    bz_sb_reset_to(arr, buf, total);
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

    // StringBuilder operates in width-1 (byte) mode; format strings are width-1
    // (the ASCII placeholder chars {, }, digits are read as raw bytes).
    const char* fb = (const char*)fmt->data;
    int64_t nb = fmt->len * (fmt->width ? fmt->width : 1);
    int64_t nargs = args ? args->count : 0;
    void** argSlots = args ? (void**)args->data : nullptr;

    // Resolve every arg's string form ONCE (virtual ToString, vtable slot 3) so
    // the two passes below share one cached {data,len} per arg — ToString is not
    // re-invoked (it allocates; calling it twice would double the tolerated leak
    // and could observe side effects twice). A null/undispatchable arg yields "".
    struct ArgStr { const char* data; int64_t len; };
    ArgStr* astr = (ArgStr*)malloc((size_t)(nargs > 0 ? nargs : 1) * sizeof(ArgStr));
    for (int64_t k = 0; k < nargs; k++) {
        astr[k].data = "";
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
        astr[k].data = (const char*)s->data;
        astr[k].len = s->len * (s->width ? s->width : 1);
    }

    // Parse one placeholder beginning at fb[i]=='{'. On success sets idx/close
    // (index of '}') and returns true; a malformed/empty index returns false.
    auto parsePlaceholder = [&](int64_t i, int64_t& idx, int64_t& close)
        -> bool {
        int64_t j = i + 1;
        if (j >= nb || fb[j] < '0' || fb[j] > '9') return false;  // need a digit
        int64_t v = 0;
        while (j < nb && fb[j] >= '0' && fb[j] <= '9') { v = v * 10 + (fb[j] - '0'); j++; }
        // Skip optional ,alignment and :formatString up to the closing brace.
        while (j < nb && fb[j] != '}') j++;
        if (j >= nb || fb[j] != '}') return false;               // no closing brace
        idx = v;
        close = j;
        return true;
    };

    // First pass: total output byte length.
    int64_t total = 0;
    for (int64_t i = 0; i < nb;) {
        char c = fb[i];
        if (c == '{') {
            if (i + 1 < nb && fb[i + 1] == '{') { total++; i += 2; continue; }
            int64_t idx = -1, close = -1;
            if (parsePlaceholder(i, idx, close)) {
                if (idx >= 0 && idx < nargs) total += astr[idx].len;
                i = close + 1;
            } else { total++; i++; }                              // literal '{'
        } else if (c == '}') {
            if (i + 1 < nb && fb[i + 1] == '}') { total++; i += 2; continue; }
            total++; i++;
        } else { total++; i++; }
    }

    // Second pass: fill the buffer.
    char* buf = (char*)malloc((size_t)(total + 1));
    int64_t off = 0;
    for (int64_t i = 0; i < nb;) {
        char c = fb[i];
        if (c == '{') {
            if (i + 1 < nb && fb[i + 1] == '{') { buf[off++] = '{'; i += 2; continue; }
            int64_t idx = -1, close = -1;
            if (parsePlaceholder(i, idx, close)) {
                if (idx >= 0 && idx < nargs && astr[idx].len > 0) {
                    memcpy(buf + off, astr[idx].data, (size_t)astr[idx].len);
                    off += astr[idx].len;
                }
                i = close + 1;
            } else { buf[off++] = fb[i++]; }
        } else if (c == '}') {
            if (i + 1 < nb && fb[i + 1] == '}') { buf[off++] = '}'; i += 2; continue; }
            buf[off++] = fb[i++];
        } else { buf[off++] = fb[i++]; }
    }
    buf[off] = '\0';
    free(astr);

    // Wrap the formatted bytes as a fresh +1 width-1 part and append it (like
    // Append — the builder does not own/retain its parts, so the allocation is
    // the existing tolerated leak; no part is freed here).
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &buf, sizeof(void*));                 // data
    memcpy(strObj + 8, &off, sizeof(int64_t));           // len
    strObj[16] = 1;                                      // width = 1 (byte mode)
    bz_string_init_rc(strObj);                           // producer's reference, rc := 1
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
