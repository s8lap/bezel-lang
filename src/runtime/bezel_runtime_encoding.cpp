// bezel_runtime_encoding.cpp — Encoding (UTF-8/16/32/ASCII).
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// Encoding (stdlib §3.2). The abstract `Encoding` base declares
// `GetBytes(s): owned Array<byte>` and `GetString(bytes): string`. (fix #3851,
// stdlib §3.2 / §1, v0.290: the windowed three-argument `GetString(bytes,
// start, length)` CALL FORM was retired at v0.290 — a window over an
// Array<byte> is a slice, not a parameter pair — and is rejected in sema
// before reaching codegen or this runtime. The `index`/`count` parameters
// below stay: the surviving one-argument `GetString(bytes)` still lowers
// through them with a synthesised full window.)
// This runtime implements the UTF-8 concrete encoding (`Encoding.Utf8`), which is
// total and lossless over every Bezel `string` (UTF-8 is the canonical byte form
// referenced throughout §3.4 / §32.5). Utf16/Utf32/Ascii are NOT implemented here
// (their concrete GetBytes/GetString semantics — endianness, surrogate handling,
// and out-of-range behaviour for Ascii — are not pinned down by §3.2); codegen
// rejects those singletons until the spec specifies them.
//
// GetBytes returns a fresh, owned `Array<byte>` (a bz_array_t* with elem_size 1):
// the caller's `owned` binding releases it via the ordinary collection-dispose
// path. GetString returns a fresh +1 managed `string` (rc := 1, exactly like
// every other heap string producer); the byte input is borrowed and untouched.

// Encoding.Utf8.GetBytes(s): walk the string's code points (width 1/2/4) and
// UTF-8-encode each into a fresh byte array. Mirrors bz_string_to_utf8z's encoder
// but emits into a bz_array_t<byte> (no trailing NUL — a byte array carries an
// explicit length, unlike the NUL-terminated FFI buffer).
extern "C" void* bz_encoding_utf8_get_bytes(const void* strp) {
    void* arr = bz_array_alloc(1, 0);  // Array<byte>: elem_size 1
    if (!strp) return arr;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        uint8_t b;
        if (cp <= 0x7F) {
            b = (uint8_t)cp;                          bz_array_add(arr, &b, 1);
        } else if (cp <= 0x7FF) {
            b = (uint8_t)(0xC0 | (cp >> 6));          bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | (cp & 0x3F));        bz_array_add(arr, &b, 1);
        } else if (cp <= 0xFFFF) {
            b = (uint8_t)(0xE0 | (cp >> 12));         bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | (cp & 0x3F));        bz_array_add(arr, &b, 1);
        } else {
            b = (uint8_t)(0xF0 | (cp >> 18));          bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));  bz_array_add(arr, &b, 1);
            b = (uint8_t)(0x80 | (cp & 0x3F));         bz_array_add(arr, &b, 1);
        }
    }
    return arr;
}

// bz_decode_utf8_range — THE strict UTF-8 decoder (there is exactly one).
//
// Decodes `nbytes` bytes at `p` into a fresh +1 managed bz.string, or returns
// NULL when the byte sequence is not well-formed UTF-8. Two callers share it so
// the well-formedness rules of stdlib §3.2 (v0.136) live in one place:
//   - bz_encoding_utf8_get_string — an Array<byte> window (index/count), the
//     §3.2 `Encoding.UTF8.GetString` family;
//   - bz_string_from_utf8z — a NUL-terminated C buffer, the language spec
//     §32.6 / stdlib §10.4 (v0.240) `string.From(c: CString)` direction.
// The caller decides what a NULL means (both currently raise FormatException).
//
// The decoded string is stored at the NARROWEST width (1/2/4) that holds every
// code point — exactly the §4.6 adaptive-storage rule the codegen literal
// emitter (codePointsWidth: ≤0xFF→1, ≤0xFFFF→2, else 4) uses. This is required,
// not merely an optimization: the string-equality codegen compares `len*width`
// bytes via memcmp and is only correct when both operands share a width
// (LlvmBackend_String.cpp comment at emitStringEquals). Producing the same width
// the literal emitter would makes `GetString(GetBytes(s)) == s` compare correctly
// and round-trip exactly.
static void* bz_decode_utf8_range(const uint8_t* p, int64_t nbytes) {
    int64_t count = nbytes < 0 ? 0 : nbytes;
    int64_t end = count;

    // Pass 1: decode into a temporary width-4 code-point buffer; track max cp.
    // Strict well-formedness per stdlib §3.2 (v0.136): any ill-formed sequence
    // — invalid lead byte, truncated tail, bad continuation byte, overlong form,
    // surrogate encoding, or a code point above U+10FFFF — is a defined fault.
    // The decoder rejects such input by freeing the buffer and returning NULL;
    // the codegen caller (tryLowerEncodingCall) turns NULL into FormatException.
    uint32_t* cps = (uint32_t*)malloc((size_t)(count > 0 ? count : 1) * 4);
    int64_t ncp = 0;
    uint32_t maxCp = 0;
    int64_t i = 0;
    while (i < end) {
        uint8_t b0 = p[i];
        uint32_t cp;
        int64_t adv;
        if (b0 < 0x80) {                       // 1-byte: 0xxxxxxx
            cp = b0; adv = 1;
        } else if ((b0 & 0xE0) == 0xC0) {      // 2-byte: 110xxxxx 10xxxxxx
            if (i + 1 >= end || (p[i + 1] & 0xC0) != 0x80) { free(cps); return nullptr; }
            cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[i + 1] & 0x3F);
            if (cp < 0x80) { free(cps); return nullptr; }  // overlong (C0/C1 leads)
            adv = 2;
        } else if ((b0 & 0xF0) == 0xE0) {      // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
            if (i + 2 >= end || (p[i + 1] & 0xC0) != 0x80 ||
                (p[i + 2] & 0xC0) != 0x80) { free(cps); return nullptr; }
            cp = ((uint32_t)(b0 & 0x0F) << 12) |
                 ((uint32_t)(p[i + 1] & 0x3F) << 6) |
                 (uint32_t)(p[i + 2] & 0x3F);
            if (cp < 0x800) { free(cps); return nullptr; }                  // overlong
            if (cp >= 0xD800 && cp <= 0xDFFF) { free(cps); return nullptr; } // surrogate
            adv = 3;
        } else if ((b0 & 0xF8) == 0xF0) {      // 4-byte: 11110xxx 10xxxxxx ×3
            if (i + 3 >= end || (p[i + 1] & 0xC0) != 0x80 ||
                (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) {
                free(cps); return nullptr;
            }
            cp = ((uint32_t)(b0 & 0x07) << 18) |
                 ((uint32_t)(p[i + 1] & 0x3F) << 12) |
                 ((uint32_t)(p[i + 2] & 0x3F) << 6) |
                 (uint32_t)(p[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) { free(cps); return nullptr; } // overlong / range
            adv = 4;
        } else {
            free(cps); return nullptr;         // invalid lead byte → FormatException
        }
        cps[ncp++] = cp;
        if (cp > maxCp) maxCp = cp;
        i += adv;
    }

    // Pass 2: select the narrowest width (§4.6 adaptive storage) and pack.
    int8_t width = (maxCp <= 0xFF) ? 1 : (maxCp <= 0xFFFF ? 2 : 4);
    void* sdata = malloc((size_t)(ncp > 0 ? ncp : 1) * (size_t)width);
    if (width == 1) {
        uint8_t* d = (uint8_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint8_t)cps[k];
    } else if (width == 2) {
        uint16_t* d = (uint16_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint16_t)cps[k];
    } else {
        uint32_t* d = (uint32_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = cps[k];
    }
    free(cps);

    // Build the bz.string header { data, len, width, rc=1 }.
    uint8_t* s = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(s, &sdata, sizeof(void*));      // data (narrowest-width buffer)
    memcpy(s + 8, &ncp, sizeof(int64_t));  // len (code-point count)
    s[16] = (uint8_t)width;                // width = 1/2/4
    bz_string_init_rc(s);                  // producer's reference, rc := 1
    return s;
}

// Encoding.Utf8.GetString(bytes, index, count): decode `count` UTF-8 bytes
// starting at `index` of the byte array into a fresh +1 managed string. The
// `index`/`count` pair models the §3.2 three-argument overload; the one-argument
// overload passes index=0 and count=Array.Length. A malformed/truncated UTF-8
// sequence is a defined fault (stdlib §3.2 v0.136): the shared decoder returns
// NULL and the codegen caller raises FormatException — no silent substitution.
extern "C" void* bz_encoding_utf8_get_string(void* arrp, int64_t index,
                                              int64_t count) {
    auto* arr = (bz_array_t*)arrp;
    const uint8_t* data = arr ? (const uint8_t*)arr->data : nullptr;
    int64_t total = arr ? arr->count : 0;
    if (index < 0) index = 0;
    if (count < 0) count = 0;
    if (index > total) index = total;
    if (index + count > total) count = total - index;
    return bz_decode_utf8_range(data + index, count);
}

// ---------------------------------------------------------------------------
// §3.2 v0.165 (#1355): GetByteCount and write_bytes helpers.
//
// GetByteCount: count bytes needed without allocating an array.
// write_bytes: encode string into a pre-allocated destination buffer.
// The caller (tryLowerEncodingCall codegen) ensures dest is large enough
// by first calling GetByteCount and bounds-checking the destination span.

extern "C" int64_t bz_encoding_utf8_get_byte_count(const void* strp) {
    if (!strp) return 0;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len; if (n < 0) n = 0;
    int64_t count = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = (s->width == 1) ? ((const uint8_t*)s->data)[i]
                    : (s->width == 2) ? ((const uint16_t*)s->data)[i]
                                      : ((const uint32_t*)s->data)[i];
        if      (cp <= 0x7F)   count += 1;
        else if (cp <= 0x7FF)  count += 2;
        else if (cp <= 0xFFFF) count += 3;
        else                   count += 4;
    }
    return count;
}

// per §3.2 v0.165 (#1355): UTF16.GetByteCount. UTF-16LE: 2 bytes per BMP code
// point, 4 bytes per supplementary code point (surrogate pair). No byte-order
// mark. Distinct from bz_encoding_utf16_get_bytes in semantics only — this
// returns the count without allocating.
extern "C" int64_t bz_encoding_utf16_get_byte_count(const void* strp) {
    if (!strp) return 0;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len; if (n < 0) n = 0;
    int64_t count = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = (s->width == 1) ? ((const uint8_t*)s->data)[i]
                    : (s->width == 2) ? ((const uint16_t*)s->data)[i]
                                      : ((const uint32_t*)s->data)[i];
        count += (cp > 0xFFFF) ? 4 : 2;  // surrogate pair vs BMP
    }
    return count;
}

// per §3.2 v0.165 (#1355): UTF32.GetByteCount. UTF-32LE: 4 bytes per code
// point (every Unicode scalar value fits in 32 bits). No byte-order mark.
extern "C" int64_t bz_encoding_utf32_get_byte_count(const void* strp) {
    if (!strp) return 0;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len; if (n < 0) n = 0;
    return n * 4;  // UTF-32LE: 4 bytes per code point
}

extern "C" int64_t bz_encoding_ascii_get_byte_count(const void* strp) {
    // Returns -1 on any non-ASCII code point (codegen turns this into
    // FormatException per the strict error model of §3.2 v0.136).
    if (!strp) return 0;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len; if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = (s->width == 1) ? ((const uint8_t*)s->data)[i]
                    : (s->width == 2) ? ((const uint16_t*)s->data)[i]
                                      : ((const uint32_t*)s->data)[i];
        if (cp > 0x7F) return -1;  // non-ASCII → defined fault
    }
    return n;  // ASCII: 1 byte per code point
}

extern "C" void bz_encoding_utf8_write_bytes(const void* strp, void* destp) {
    if (!strp || !destp) return;
    const auto* s = (const bz_string_struct*)strp;
    uint8_t* d = (uint8_t*)destp;
    int64_t n = s->len; if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = (s->width == 1) ? ((const uint8_t*)s->data)[i]
                    : (s->width == 2) ? ((const uint16_t*)s->data)[i]
                                      : ((const uint32_t*)s->data)[i];
        if (cp <= 0x7F) {
            *d++ = (uint8_t)cp;
        } else if (cp <= 0x7FF) {
            *d++ = (uint8_t)(0xC0 | (cp >> 6));
            *d++ = (uint8_t)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            *d++ = (uint8_t)(0xE0 | (cp >> 12));
            *d++ = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            *d++ = (uint8_t)(0x80 | (cp & 0x3F));
        } else {
            *d++ = (uint8_t)(0xF0 | (cp >> 18));
            *d++ = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            *d++ = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            *d++ = (uint8_t)(0x80 | (cp & 0x3F));
        }
    }
}

// per §3.2 v0.165 (#1355): UTF16 encode into a pre-allocated destination
// buffer. Caller has pre-sized the buffer via bz_encoding_utf16_get_byte_count.
// BMP code points → 2 bytes LE; supplementary code points → 4 bytes LE
// surrogate pair (hi, lo). No BOM emitted. Dest size = exactly
// bz_encoding_utf16_get_byte_count(strp) bytes; IndexOutOfRangeException if
// shorter (raised at the caller's buffer-bounds check, not here).
extern "C" void bz_encoding_utf16_write_bytes(const void* strp, void* destp) {
    if (!strp || !destp) return;
    const auto* s = (const bz_string_struct*)strp;
    uint8_t* d = (uint8_t*)destp;
    int64_t n = s->len; if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = (s->width == 1) ? ((const uint8_t*)s->data)[i]
                    : (s->width == 2) ? ((const uint16_t*)s->data)[i]
                                      : ((const uint32_t*)s->data)[i];
        if (cp <= 0xFFFF) {
            // BMP: little-endian 16-bit
            d[0] = (uint8_t)(cp & 0xFF);
            d[1] = (uint8_t)((cp >> 8) & 0xFF);
            d += 2;
        } else {
            // Supplementary: UTF-16LE surrogate pair
            cp -= 0x10000u;
            uint16_t hi = (uint16_t)(0xD800u + (cp >> 10));
            uint16_t lo = (uint16_t)(0xDC00u + (cp & 0x3FFu));
            d[0] = (uint8_t)(hi & 0xFF); d[1] = (uint8_t)((hi >> 8) & 0xFF); d += 2;
            d[0] = (uint8_t)(lo & 0xFF); d[1] = (uint8_t)((lo >> 8) & 0xFF); d += 2;
        }
    }
}

// per §3.2 v0.165 (#1355): UTF32 encode into a pre-allocated destination
// buffer. Every Unicode scalar value fits in 32 bits → 4 bytes LE per code
// point. No BOM emitted. Dest size = exactly bz_encoding_utf32_get_byte_count
// (strp) bytes.
extern "C" void bz_encoding_utf32_write_bytes(const void* strp, void* destp) {
    if (!strp || !destp) return;
    const auto* s = (const bz_string_struct*)strp;
    uint8_t* d = (uint8_t*)destp;
    int64_t n = s->len; if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = (s->width == 1) ? ((const uint8_t*)s->data)[i]
                    : (s->width == 2) ? ((const uint16_t*)s->data)[i]
                                      : ((const uint32_t*)s->data)[i];
        // Little-endian 32-bit code point
        d[0] = (uint8_t)(cp & 0xFF);
        d[1] = (uint8_t)((cp >> 8) & 0xFF);
        d[2] = (uint8_t)((cp >> 16) & 0xFF);
        d[3] = (uint8_t)((cp >> 24) & 0xFF);
        d += 4;
    }
}

extern "C" void bz_encoding_ascii_write_bytes(const void* strp, void* destp) {
    // Caller has already verified GetByteCount != -1 (all code points ≤ U+007F).
    if (!strp || !destp) return;
    const auto* s = (const bz_string_struct*)strp;
    uint8_t* d = (uint8_t*)destp;
    int64_t n = s->len; if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = (s->width == 1) ? ((const uint8_t*)s->data)[i]
                    : (s->width == 2) ? ((const uint16_t*)s->data)[i]
                                      : ((const uint32_t*)s->data)[i];
        *d++ = (uint8_t)(cp & 0x7Fu);
    }
}

// ---------------------------------------------------------------------------
// Encoding: Utf16 / Utf32 / Ascii (stdlib §3.2, v0.136).
//
// All three follow the strict error model: malformed or unrepresentable input
// is a defined fault — FormatException — not a silent substitution.  The
// codegen callers (tryLowerEncodingCall) check the return value for NULL and
// emit emitGuardThrow(null_check, "FormatException").
//
// Return convention:
//   GetBytes: returns a fresh owned bz_array_t* on success, NULL on error.
//   GetString: returns a fresh +1 managed bz.string* on success, NULL on error.
//
// Utf16 = UTF-16LE (stdlib §3.2 v0.136):
//   GetBytes:  never fails (every Unicode scalar value is representable; BMP
//              chars → 2 bytes LE, supplementary → 4 bytes LE surrogate pair).
//   GetString: requires even byte count, consumes one leading LE BOM (FF FE)
//              if present, raises FormatException on odd count, a big-endian
//              BOM (FE FF), or an unpaired surrogate.
//
// Utf32 = UTF-32LE (stdlib §3.2 v0.136):
//   GetBytes:  never fails.
//   GetString: requires byte count divisible by 4, consumes one leading LE BOM
//              if present, raises FormatException on non-scalar-value code
//              points (> U+10FFFF or in the surrogate range U+D800..U+DFFF).
//
// Ascii = strict 7-bit (stdlib §3.2 v0.136):
//   GetBytes:  raises FormatException on any code point above U+007F.
//   GetString: raises FormatException on any byte above 0x7F.

extern "C" void* bz_encoding_utf16_get_bytes(const void* strp) {
    // Utf16 GetBytes: encode each code point as UTF-16LE.  Bezel strings carry
    // native Unicode code points; emit BMP chars as 2-byte LE, supplementary
    // chars as 4-byte LE surrogate pair.  No BOM emitted (stdlib §3.2 v0.136).
    // Returns a fresh owned Array<byte> (elem_size 1).  Never fails.
    void* arr = bz_array_alloc(1, 0);
    if (!strp) return arr;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        if (cp <= 0xFFFF) {
            // BMP: emit as 2-byte LE unit.
            uint8_t b[2] = { (uint8_t)(cp & 0xFF), (uint8_t)(cp >> 8) };
            bz_array_add(arr, &b[0], 1);
            bz_array_add(arr, &b[1], 1);
        } else {
            // Supplementary: encode as surrogate pair.
            cp -= 0x10000;
            uint16_t hi = (uint16_t)(0xD800 | (cp >> 10));
            uint16_t lo = (uint16_t)(0xDC00 | (cp & 0x3FF));
            uint8_t b[4] = { (uint8_t)(hi & 0xFF), (uint8_t)(hi >> 8),
                             (uint8_t)(lo & 0xFF), (uint8_t)(lo >> 8) };
            bz_array_add(arr, &b[0], 1); bz_array_add(arr, &b[1], 1);
            bz_array_add(arr, &b[2], 1); bz_array_add(arr, &b[3], 1);
        }
    }
    return arr;
}

// bz_decode_utf16le_range — THE strict UTF-16LE decoder (there is exactly one).
//
// Decodes `nbytes` bytes of UTF-16LE code units at `p` into a fresh +1 managed
// bz.string, or returns NULL on an unpaired/truncated surrogate. Reads
// byte-wise, so `p` need not be 2-byte aligned. Deliberately applies NO
// byte-order-mark policy and no byte-count parity check: those belong to the
// §3.2 `Encoding.UTF16` contract, not to the §32.6 FFI read-back, and the
// callers differ on them (see bz_string_from_utf16z). Two callers share it so
// the surrogate rules live in one place:
//   - bz_encoding_utf16_get_string — stdlib §3.2 (v0.136), after its own parity
//     and BOM handling;
//   - bz_string_from_utf16z — language spec §32.6 / stdlib §10.4 (v0.240).
static void* bz_decode_utf16le_range(const uint8_t* p, int64_t nbytes) {
    int64_t nBytes = nbytes < 0 ? 0 : nbytes;
    int64_t pos = 0;

    // Decode UTF-16LE units into a temporary code-point buffer.
    // Maximum code points = nBytes/2 (surrogates pair into one cp, so actual ≤ nBytes/2).
    int64_t maxCp = (nBytes - pos) / 2;
    uint32_t* cps = (uint32_t*)malloc((size_t)(maxCp > 0 ? maxCp : 1) * 4);
    int64_t ncp = 0;
    uint32_t maxVal = 0;

    while (pos + 2 <= nBytes) {
        uint16_t unit = (uint16_t)(p[pos] | (p[pos + 1] << 8));
        pos += 2;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // High surrogate: must be followed by a low surrogate.
            if (pos + 2 > nBytes) { free(cps); return nullptr; }
            uint16_t lo = (uint16_t)(p[pos] | (p[pos + 1] << 8));
            pos += 2;
            if (lo < 0xDC00 || lo > 0xDFFF) { free(cps); return nullptr; }
            uint32_t cp = 0x10000 + (((uint32_t)(unit & 0x3FF)) << 10) +
                          (uint32_t)(lo & 0x3FF);
            cps[ncp++] = cp;
            if (cp > maxVal) maxVal = cp;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            // Unpaired low surrogate → FormatException.
            free(cps); return nullptr;
        } else {
            cps[ncp++] = (uint32_t)unit;
            if ((uint32_t)unit > maxVal) maxVal = (uint32_t)unit;
        }
    }

    // Pack into adaptive-width string (§4.6).
    int8_t width = (maxVal <= 0xFF) ? 1 : (maxVal <= 0xFFFF ? 2 : 4);
    void* sdata = malloc((size_t)(ncp > 0 ? ncp : 1) * (size_t)width);
    if (width == 1) {
        uint8_t* d = (uint8_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint8_t)cps[k];
    } else if (width == 2) {
        uint16_t* d = (uint16_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint16_t)cps[k];
    } else {
        uint32_t* d = (uint32_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = cps[k];
    }
    free(cps);

    uint8_t* str = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(str, &sdata, sizeof(void*));
    memcpy(str + 8, &ncp, sizeof(int64_t));
    str[16] = (uint8_t)width;
    bz_string_init_rc(str);
    return str;
}

// Utf16 GetString(bytes, index, count): decode `count` UTF-16LE bytes starting
// at `index` of the byte array.  Consumes one leading LE BOM if present.
// Returns NULL (→ FormatException) on odd count, big-endian BOM, or unpaired
// surrogate.  On success returns a fresh +1 managed bz.string*.
extern "C" void* bz_encoding_utf16_get_string(void* arrp, int64_t index,
                                               int64_t count) {
    auto* arr = (bz_array_t*)arrp;
    const uint8_t* data = arr ? (const uint8_t*)arr->data : nullptr;
    int64_t total = arr ? arr->count : 0;
    if (index < 0) index = 0;
    if (count < 0) count = 0;
    if (index > total) index = total;
    if (index + count > total) count = total - index;
    // Odd byte count → FormatException.
    if (count % 2 != 0) return nullptr;
    const uint8_t* p = data + index;
    int64_t pos = 0;

    // Check for and consume leading BOM (FF FE = LE, allowed; FE FF = BE, error).
    if (count >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) {
            pos = 2;  // consume LE BOM
        } else if (p[0] == 0xFE && p[1] == 0xFF) {
            return nullptr;  // big-endian BOM → FormatException
        }
    }
    return bz_decode_utf16le_range(p + pos, count - pos);
}

// ---------------------------------------------------------------------------
// FFI read-back: C buffer → Bezel string (language spec §32.6, stdlib §10.4,
// v0.240; fix #2734). The inverse of bz_string_to_utf8z / bz_string_to_utf16z.
//
// "`string.From(c)` reads from a caller-owned `CString` and produces a Bezel
// `string`; the original is unaffected and is still subject to the caller's
// disposal obligation" (spec §32.6). Both functions therefore only READ the
// caller's buffer: no free, no retention — the returned string owns a fresh
// copy, so the source may be disposed independently (§28.3 exactly-once).
//
// Both route through the shared strict decoders above, so an ill-formed buffer
// is the §3.2 defined fault rather than a silent substitution: they return NULL
// and the codegen caller raises FormatException. NULL in → NULL out; the
// pointer forms guard IntPtr.Zero at the call site (§10.4 requires
// ArgumentNullException there, which is a different exception).

// string.From(c: CString) / string.FromCStringPointer(p): copy the UTF-8 bytes
// up to (not including) the NUL terminator into a fresh Bezel string.
extern "C" void* bz_string_from_utf8z(const void* p) {
    if (!p) return nullptr;
    return bz_decode_utf8_range((const uint8_t*)p,
                                (int64_t)strlen((const char*)p));
}

// string.From(w: WString) / string.FromWStringPointer(p): copy the UTF-16LE
// code units up to (not including) the terminating 0x0000 unit into a fresh
// Bezel string. bz_wstrlen counts UNITS (strlen would stop at the high zero
// byte of an ASCII code unit), so the byte count is 2× that.
//
// No BOM is consumed here, unlike Encoding.UTF16.GetString: §10.4 says the
// conversion "reads ... 16-bit code units starting at the pointer until a null
// terminator is found" and §32.6 says the copy is of the buffer's contents, so
// a leading U+FEFF is an ordinary code point of the string — consuming it would
// make string.From(WString.From(s)) unequal to s for such an s.
extern "C" void* bz_string_from_utf16z(const void* p) {
    if (!p) return nullptr;
    return bz_decode_utf16le_range((const uint8_t*)p, bz_wstrlen(p) * 2);
}

// Utf32 GetBytes: encode each code point as 4-byte LE.  No BOM emitted.
// Never fails (all Bezel code points are valid Unicode scalar values).
extern "C" void* bz_encoding_utf32_get_bytes(const void* strp) {
    void* arr = bz_array_alloc(1, 0);
    if (!strp) return arr;
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        uint8_t b[4] = { (uint8_t)(cp & 0xFF), (uint8_t)((cp >> 8) & 0xFF),
                         (uint8_t)((cp >> 16) & 0xFF), (uint8_t)((cp >> 24) & 0xFF) };
        bz_array_add(arr, &b[0], 1); bz_array_add(arr, &b[1], 1);
        bz_array_add(arr, &b[2], 1); bz_array_add(arr, &b[3], 1);
    }
    return arr;
}

// Utf32 GetString(bytes, index, count): decode `count` UTF-32LE bytes.
// Requires byte count divisible by 4; consumes one leading LE BOM (00 00 FE FF
// in LE byte order = bytes 0xFF,0xFE,0x00,0x00).  Returns NULL on non-divisible
// count or any code unit that is not a Unicode scalar value (> U+10FFFF or in
// the surrogate range U+D800..U+DFFF).
extern "C" void* bz_encoding_utf32_get_string(void* arrp, int64_t index,
                                               int64_t count) {
    auto* arr = (bz_array_t*)arrp;
    const uint8_t* data = arr ? (const uint8_t*)arr->data : nullptr;
    int64_t total = arr ? arr->count : 0;
    if (index < 0) index = 0;
    if (count < 0) count = 0;
    if (index > total) index = total;
    if (index + count > total) count = total - index;
    // Count must be divisible by 4.
    if (count % 4 != 0) return nullptr;
    const uint8_t* p = data + index;
    int64_t nBytes = count;
    int64_t pos = 0;

    // Check for and consume leading LE BOM (0xFF 0xFE 0x00 0x00 = U+FEFF in LE).
    if (nBytes >= 4 &&
        p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00) {
        pos = 4;  // consume LE BOM
    }

    int64_t nUnits = (nBytes - pos) / 4;
    uint32_t* cps = (uint32_t*)malloc((size_t)(nUnits > 0 ? nUnits : 1) * 4);
    int64_t ncp = 0;
    uint32_t maxVal = 0;

    while (pos + 4 <= nBytes) {
        uint32_t cp = (uint32_t)(p[pos]) | ((uint32_t)(p[pos+1]) << 8) |
                      ((uint32_t)(p[pos+2]) << 16) | ((uint32_t)(p[pos+3]) << 24);
        pos += 4;
        // Validate: must be a Unicode scalar value.
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            free(cps); return nullptr;
        }
        cps[ncp++] = cp;
        if (cp > maxVal) maxVal = cp;
    }

    int8_t width = (maxVal <= 0xFF) ? 1 : (maxVal <= 0xFFFF ? 2 : 4);
    void* sdata = malloc((size_t)(ncp > 0 ? ncp : 1) * (size_t)width);
    if (width == 1) {
        uint8_t* d = (uint8_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint8_t)cps[k];
    } else if (width == 2) {
        uint16_t* d = (uint16_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = (uint16_t)cps[k];
    } else {
        uint32_t* d = (uint32_t*)sdata;
        for (int64_t k = 0; k < ncp; ++k) d[k] = cps[k];
    }
    free(cps);

    uint8_t* str = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(str, &sdata, sizeof(void*));
    memcpy(str + 8, &ncp, sizeof(int64_t));
    str[16] = (uint8_t)width;
    bz_string_init_rc(str);
    return str;
}

// Ascii GetBytes: strict 7-bit.  Returns NULL (→ FormatException) if any code
// point is above U+007F.  Otherwise returns a fresh owned Array<byte>.
extern "C" void* bz_encoding_ascii_get_bytes(const void* strp) {
    if (!strp) return bz_array_alloc(1, 0);
    const auto* s = (const bz_string_struct*)strp;
    int64_t n = s->len;
    if (n < 0) n = 0;
    // Pre-check: any non-ASCII code point → fail.
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp;
        if (s->width == 1)      cp = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) cp = ((const uint16_t*)s->data)[i];
        else                    cp = ((const uint32_t*)s->data)[i];
        if (cp > 0x7F) return nullptr;  // → FormatException
    }
    void* arr = bz_array_alloc(1, (int)n);
    for (int64_t i = 0; i < n; ++i) {
        uint8_t b;
        if (s->width == 1) b = ((const uint8_t*)s->data)[i];
        else if (s->width == 2) b = (uint8_t)((const uint16_t*)s->data)[i];
        else b = (uint8_t)((const uint32_t*)s->data)[i];
        bz_array_add(arr, &b, 1);
    }
    return arr;
}

// Ascii GetString(bytes, index, count): strict 7-bit decode.  Returns NULL
// (→ FormatException) if any byte is above 0x7F.  Otherwise returns a fresh
// +1 managed bz.string* with width=1.
extern "C" void* bz_encoding_ascii_get_string(void* arrp, int64_t index,
                                               int64_t count) {
    auto* arr = (bz_array_t*)arrp;
    const uint8_t* data = arr ? (const uint8_t*)arr->data : nullptr;
    int64_t total = arr ? arr->count : 0;
    if (index < 0) index = 0;
    if (count < 0) count = 0;
    if (index > total) index = total;
    if (index + count > total) count = total - index;
    const uint8_t* p = data + index;
    // Pre-check: any byte above 0x7F → FormatException.
    for (int64_t i = 0; i < count; ++i) {
        if (p[i] > 0x7F) return nullptr;
    }
    // All bytes are valid ASCII (== code points 0–127, width 1).
    void* sdata = malloc((size_t)(count > 0 ? count : 1));
    memcpy(sdata, p, (size_t)count);
    int64_t ncp = count;
    uint8_t* str = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(str, &sdata, sizeof(void*));
    memcpy(str + 8, &ncp, sizeof(int64_t));
    str[16] = 1;  // width = 1 (ASCII fits in one byte)
    bz_string_init_rc(str);
    return str;
}
