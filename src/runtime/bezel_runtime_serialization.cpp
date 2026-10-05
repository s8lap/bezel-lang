// bezel_runtime_serialization.cpp — JSON + binary (de)serialization, tier-3 identity-tag graphs.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ============================================================
// BinarySerializer / BinaryDeserializer runtime  (§2.15 / §9.8)
// ============================================================
// All extern "C" function signatures match bezel_runtime.h exactly.
// Internal types use the bz_array_t / bz_string_struct layouts defined
// earlier in this file.

// --- writer state -----------------------------------------------------------

// §9.8 / tier-3 graph serialization: wire markers for the identity-tag protocol.
// Written before each class-typed component in `serialized graph` mode:
//   BZ_SER_INLINE (0x01) + LE32 tag_id  → first owning reach of this object
//   BZ_SER_REF    (0x02) + LE32 tag_id  → back-edge / shared reference
// Under `serialized tree` mode these markers are NOT emitted (legacy path).
#define BZ_SER_INLINE 0x01
#define BZ_SER_REF    0x02

// JSON serializer state (used when kind == 1).
// The output is accumulated in buf/used/cap (same as binary, but UTF-8 JSON text).
// Nesting depth is tracked for comma logic; `needs_comma[d]` = true when the
// current value at depth d needs a comma written before it.
// Depth limit: BZ_JSER_MAX_DEPTH (must match §2.15 MaxDepth default = 64).
#define BZ_JSER_MAX_DEPTH 64
struct bz_jser_state_t {
    int     depth;                         // current nesting depth
    bool    needs_comma[BZ_JSER_MAX_DEPTH]; // does a comma precede the next value?
    // fix #687 / stdlib §2.15 JsonOptions:
    bool    indented;           // pretty-print with newlines
    int32_t indent_width;       // spaces per indent level (default 2)
    int32_t max_depth;          // nesting guard: 0 = unlimited, >0 = raise when exceeded
    bool    after_field_name;   // true after WriteFieldName; skip extra indent for value
};

struct bz_binser_t {
    uint8_t* buf;
    int64_t  used;   // bytes written so far
    int64_t  cap;    // allocated capacity
    int8_t   kind;   // 0 = binary (default), 1 = JSON

    // JSON state (only valid when kind == 1).
    bz_jser_state_t json;

    // §9.8 tier-3: identity-tag table (pointer → tag_id) for `serialized graph`.
    // tag_ptrs[i] = the object pointer for tag_id (i+1).  tag_id=0 means "not seen".
    // The table is allocated on first use (null when no graph serialization active).
    void**   tag_ptrs;
    int32_t  tag_count;  // number of assigned tags (= tag_ptrs size in use)
    int32_t  tag_cap;    // allocated capacity of tag_ptrs

    // §2.15 v0.139 JSON: pending inline tag_id to emit as `"$id":n` at the start
    // of the next begin_object call.  write_inline_tag stores the id here; zero
    // means no pending tag.  This defers the emission until after `{` is written.
    int32_t  json_pending_tag_id;
};

// fix #1762 finding R5: realloc-into-same-pointer leaks on failure — keep the
// original buffer intact instead of overwriting it with NULL.
//
// A failed growth TERMINATES per spec §31.4 ("collection growth" / "string
// construction"). The serializer does not bound-check on write, so every other
// posture is unsafe: the first cut of this fix returned void and documented the
// resulting overrun as acceptable ("strictly a leak fix"), which turns a
// deterministic NULL-deref crash into a silent heap-buffer-overflow; dropping
// the write instead would emit a silently TRUNCATED payload that no reader can
// distinguish from a short one.
static void bz_binser_ensure(bz_binser_t* s, int64_t need) {
    if (s->used + need <= s->cap) return;
    int64_t newcap = s->cap == 0 ? 256 : s->cap;
    while (newcap < s->used + need) newcap *= 2;
    void* tmp = realloc(s->buf, (size_t)newcap);
    if (!tmp) bz_alloc_fail("serializer buffer growth");
    s->buf = static_cast<uint8_t*>(tmp);
    s->cap = newcap;
}

static void bz_binser_write_byte_impl(bz_binser_t* s, uint8_t b) {
    bz_binser_ensure(s, 1);
    s->buf[s->used++] = b;
}

static void bz_binser_write_le32_impl(bz_binser_t* s, uint32_t v) {
    bz_binser_ensure(s, 4);
    s->buf[s->used+0] = (uint8_t)(v);
    s->buf[s->used+1] = (uint8_t)(v >> 8);
    s->buf[s->used+2] = (uint8_t)(v >> 16);
    s->buf[s->used+3] = (uint8_t)(v >> 24);
    s->used += 4;
}

static void bz_binser_write_le64_impl(bz_binser_t* s, uint64_t v) {
    bz_binser_ensure(s, 8);
    for (int i = 0; i < 8; ++i)
        s->buf[s->used+i] = (uint8_t)(v >> (8*i));
    s->used += 8;
}

// Write LE32 length + raw UTF-8 bytes (no NUL), per §2.15 wire format.
static void bz_binser_write_utf8_bytes(bz_binser_t* s, const char* data, int64_t byte_len) {
    bz_binser_write_le32_impl(s, (uint32_t)byte_len);
    if (byte_len > 0) {
        bz_binser_ensure(s, byte_len);
        memcpy(s->buf + s->used, data, (size_t)byte_len);
        s->used += byte_len;
    }
}

extern "C" {

void* bz_binser_alloc(void) {
    auto* s = static_cast<bz_binser_t*>(calloc(1, sizeof(bz_binser_t)));
    s->kind      = 0;  // binary
    return s;
}

// JSON serializer: same bz_binser_t, kind = 1.
void* bz_jser_alloc(void) {
    auto* s = static_cast<bz_binser_t*>(calloc(1, sizeof(bz_binser_t)));
    s->kind = 1;  // JSON
    // Defaults per stdlib §2.15: Indented=false, IndentWidth=2, MaxDepth=64.
    s->json.indented     = false;
    s->json.indent_width = 2;
    s->json.max_depth    = 64;
    return s;
}

// fix #687 / stdlib §2.15 JsonOptions: allocate with explicit options.
void* bz_jser_alloc_opts(int8_t indented, int32_t indent_width, int32_t max_depth) {
    auto* s = static_cast<bz_binser_t*>(calloc(1, sizeof(bz_binser_t)));
    s->kind = 1;  // JSON
    s->json.indented     = (indented != 0);
    s->json.indent_width = (indent_width > 0) ? indent_width : 2;
    s->json.max_depth    = (max_depth > 0) ? max_depth : 64;
    return s;
}

void bz_binser_free(void* serp) {
    if (!serp) return;
    auto* s = static_cast<bz_binser_t*>(serp);
    free(s->buf);
    free(s->tag_ptrs);
    free(s);
}

// Transfer accumulated bytes as a fresh OwningBuffer<byte> (bz_array_t* with
// elem_size=1).  The serializer's internal buffer is cleared after the copy
// so further writes start fresh.  Caller owns the returned array.
void* bz_binser_to_bytes(void* serp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    int64_t len = s->used;
    // Allocate a bz_array_t and bulk-copy the bytes directly.
    void* arr = bz_array_alloc(1, len > 0 ? len : 1);
    if (len > 0) {
        auto* a = static_cast<bz_array_t*>(arr);
        memcpy(a->data, s->buf, (size_t)len);
        a->count = len;
    }
    // Clear internal buffer so the serializer can be reused.
    s->used = 0;
    return arr;
}

// bz_jser_to_bytes: transfer JSON bytes as an OwningBuffer<byte>.
// Same logic as bz_binser_to_bytes but for JSON kind.
void* bz_jser_to_bytes(void* serp) {
    return bz_binser_to_bytes(serp);  // same buffer layout; kind tag is ignored
}

// bz_jser_to_string: decode the UTF-8 JSON bytes into a Bezel string (bz_string_struct*).
// Returns a newly-allocated bz_string_struct*; caller owns it (rc = 1).
void* bz_jser_to_string(void* serp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (!s || s->used == 0) {
        // Return empty string.
        uint8_t* strObj = static_cast<uint8_t*>(calloc(1, (size_t)BZ_STRING_SIZEOF));
        if (!strObj) return nullptr;
        // data = null (empty string has no data), len = 0, width = 1
        strObj[16] = 1;
        bz_string_init_rc(strObj);
        return strObj;
    }
    // Build a bz_string from the UTF-8 bytes in s->buf.
    // bz_binder_read_bz_string is not directly callable here (it needs a binder);
    // re-implement the UTF-8 → code-point decode inline.
    const uint8_t* src    = s->buf;
    int64_t        byte_len = s->used;

    // Pass 1: count code points + find max.
    uint32_t* cps = static_cast<uint32_t*>(malloc((size_t)byte_len * sizeof(uint32_t) + 4));
    if (!cps) return nullptr;
    int64_t ncp = 0;
    uint32_t max_cp = 0;
    int64_t i = 0;
    while (i < byte_len) {
        uint8_t b0 = src[i];
        uint32_t cp;
        int seq;
        if      (b0 < 0x80) { cp = b0;             seq = 1; }
        else if (b0 < 0xC0) { cp = 0xFFFD;         seq = 1; }
        else if (b0 < 0xE0) { cp = b0 & 0x1F;      seq = 2; }
        else if (b0 < 0xF0) { cp = b0 & 0x0F;      seq = 3; }
        else                 { cp = b0 & 0x07;      seq = 4; }
        for (int k = 1; k < seq && i + k < byte_len; ++k)
            cp = (cp << 6) | (src[i + k] & 0x3F);
        i += seq;
        cps[ncp++] = cp;
        if (cp > max_cp) max_cp = cp;
    }
    uint8_t width = (max_cp <= 0xFF) ? 1 : (max_cp <= 0xFFFF) ? 2 : 4;
    size_t data_bytes = (size_t)ncp * width + width;
    uint8_t* buf2 = static_cast<uint8_t*>(malloc(data_bytes));
    if (!buf2) { free(cps); return nullptr; }
    memset(buf2 + (size_t)ncp * width, 0, width);
    for (int64_t j = 0; j < ncp; ++j) {
        if (width == 1)       buf2[j]          = (uint8_t)cps[j];
        else if (width == 2) { uint16_t v = (uint16_t)cps[j]; memcpy(buf2 + j*2, &v, 2); }
        else                 { uint32_t v = cps[j];            memcpy(buf2 + j*4, &v, 4); }
    }
    free(cps);
    uint8_t* strObj = static_cast<uint8_t*>(calloc(1, (size_t)BZ_STRING_SIZEOF));
    if (!strObj) { free(buf2); return nullptr; }
    void* buf2p = buf2;
    memcpy(strObj,      &buf2p, sizeof(void*));
    memcpy(strObj + 8,  &ncp,   sizeof(int64_t));
    strObj[16] = width;
    bz_string_init_rc(strObj);
    return strObj;
}

// ---------------------------------------------------------------------------
// JSON serializer helpers (kind == 1)
//
// §2.15 JSON text rules:
//   WriteInt/WriteUInt → JSON number (decimal digits, no decimal point)
//   WriteFloat → JSON number; NaN/±Infinity → SerializationException
//   WriteDecimal → JSON number without precision loss (printed as decimal)
//   WriteBool → true / false
//   WriteString → quoted, escaped JSON string (\", \\, \n, \r, \t, \uXXXX)
//   WriteChar → one-character JSON string
//   WriteNull → null
//   BeginObject → { (fieldCount advisory, ignored)
//   WriteFieldName → "name":
//   EndObject → }
//   BeginArray → [
//   EndArray → ]
// ---------------------------------------------------------------------------

// Append raw bytes to the serializer buffer (same as binary path).
static void bz_jser_raw(bz_binser_t* s, const char* p, int64_t n) {
    bz_binser_ensure(s, n);
    memcpy(s->buf + s->used, p, (size_t)n);
    s->used += n;
}

// Append a NUL-terminated C string.
static void bz_jser_cstr(bz_binser_t* s, const char* p) {
    bz_jser_raw(s, p, (int64_t)strlen(p));
}

// fix #687 / stdlib §2.15 Indented: emit newline + `depth * indent_width` spaces.
// Only emitted when s->json.indented is true.
static void bz_jser_indent(bz_binser_t* s) {
    if (!s->json.indented) return;
    bz_jser_raw(s, "\n", 1);
    int spaces = s->json.depth * s->json.indent_width;
    for (int i = 0; i < spaces; ++i) bz_jser_raw(s, " ", 1);
}

// If the current depth context needs a comma, write it now and clear the flag.
// This must be called before every value (but NOT before WriteFieldName —
// commas before field names are handled in WriteFieldName itself).
// fix #687: in indented mode, emit newline+indent before array elements
// (but NOT before object-field values, which come right after "fieldname": ).
static void bz_jser_maybe_comma(bz_binser_t* s) {
    int d = s->json.depth;
    if (d >= 0 && d < BZ_JSER_MAX_DEPTH && s->json.needs_comma[d]) {
        bz_jser_cstr(s, ",");
    }
    // In indented mode: emit newline+indent before array elements.
    // after_field_name is set by bz_binser_write_field_name and cleared here,
    // so object field values (which follow ": ") don't get an extra newline.
    if (s->json.indented && d > 0 && !s->json.after_field_name) {
        bz_jser_indent(s);
    }
    s->json.after_field_name = false;
}

// After writing a value at current depth, mark that the NEXT value needs a comma.
static void bz_jser_mark_comma(bz_binser_t* s) {
    int d = s->json.depth;
    if (d >= 0 && d < BZ_JSER_MAX_DEPTH)
        s->json.needs_comma[d] = true;
}

// Write a quoted, escaped JSON string from a bz_string*.
// §2.15: "WriteString produces a quoted, escaped JSON string."
// JSON escaping rules (RFC 8259 §7):
//   " → \"   \ → \\   \n → \n   \r → \r   \t → \t
//   control chars (U+0000..U+001F, except above) → \uXXXX
static void bz_jser_write_string_impl(bz_binser_t* s, void* strp) {
    bz_jser_cstr(s, "\"");
    if (!strp) { bz_jser_cstr(s, "\""); return; }

    // Decode codepoints from bz_string (adaptive width).
    const auto* str = static_cast<const bz_string_struct*>(strp);
    int64_t  len   = str->len;
    uint8_t  width = str->width;
    const uint8_t* data = static_cast<const uint8_t*>(str->data);

    for (int64_t i = 0; i < len; ++i) {
        uint32_t cp;
        if (width == 1)      cp = data[i];
        else if (width == 2) { uint16_t v; memcpy(&v, data + i*2, 2); cp = v; }
        else                 { uint32_t v; memcpy(&v, data + i*4, 4); cp = v; }

        if (cp == '"')       { bz_jser_raw(s, "\\\"", 2); }
        else if (cp == '\\') { bz_jser_raw(s, "\\\\", 2); }
        else if (cp == '\n') { bz_jser_raw(s, "\\n",  2); }
        else if (cp == '\r') { bz_jser_raw(s, "\\r",  2); }
        else if (cp == '\t') { bz_jser_raw(s, "\\t",  2); }
        else if (cp < 0x20) {
            // Other control characters: \uXXXX
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04X", (unsigned)cp);
            bz_jser_raw(s, esc, 6);
        } else if (cp < 0x80) {
            // ASCII non-control
            char c = (char)cp;
            bz_jser_raw(s, &c, 1);
        } else {
            // Multi-byte UTF-8 encode back
            char utf8[5] = {};
            int n = 0;
            if      (cp < 0x800)   { utf8[n++]=(char)(0xC0|(cp>>6));   utf8[n++]=(char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { utf8[n++]=(char)(0xE0|(cp>>12));  utf8[n++]=(char)(0x80|((cp>>6)&0x3F));  utf8[n++]=(char)(0x80|(cp&0x3F)); }
            else                   { utf8[n++]=(char)(0xF0|(cp>>18));  utf8[n++]=(char)(0x80|((cp>>12)&0x3F)); utf8[n++]=(char)(0x80|((cp>>6)&0x3F)); utf8[n++]=(char)(0x80|(cp&0x3F)); }
            bz_jser_raw(s, utf8, n);
        }
    }
    bz_jser_cstr(s, "\"");
}

// ISerializer method implementations ----------------------------------------

void bz_binser_write_int(void* serp, int64_t v) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        bz_jser_maybe_comma(s);
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%" PRId64, v);
        bz_jser_raw(s, buf, n);
        bz_jser_mark_comma(s);
        return;
    }
    bz_binser_write_le64_impl(s, (uint64_t)v);
}

void bz_binser_write_uint(void* serp, uint64_t v) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        bz_jser_maybe_comma(s);
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%" PRIu64, v);
        bz_jser_raw(s, buf, n);
        bz_jser_mark_comma(s);
        return;
    }
    bz_binser_write_le64_impl(s, v);
}

void bz_binser_write_float(void* serp, double v) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        // §2.15: NaN / ±Infinity → SerializationException.
        if (std::isnan(v) || std::isinf(v)) {
            bz_ser_throw("Non-finite float cannot be represented in JSON (§2.15)",
                         nullptr);
            return;
        }
        bz_jser_maybe_comma(s);
        // Print with enough precision to round-trip exactly.
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%.17g", v);
        bz_jser_raw(s, buf, n);
        bz_jser_mark_comma(s);
        return;
    }
    uint64_t bits;
    memcpy(&bits, &v, 8);
    bz_binser_write_le64_impl(s, bits);
}

// fix #665 / §2.14 / §9.8: WriteDecimal — wire format is 4×u32 LE (the
// bz.decimal struct layout: lo32, mid32, hi32, flags — same as .NET Decimal
// System.Decimal binary representation).  `val` is a ptr to a 16-byte
// {i32, i32, i32, i32} struct (codegen spills to an alloca, passes address).
void bz_binser_write_decimal(void* serp, void* val) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        // §2.15: WriteDecimal → JSON number written without precision loss.
        bz_jser_maybe_comma(s);
        if (!val) {
            bz_jser_cstr(s, "0");
            bz_jser_mark_comma(s);
            return;
        }
        // bz_decimal_t layout: { lo32, mid32, hi32, flags }
        // flags bit31=sign, bits[16..23]=scale (0..28)
        // value = (hi32*2^64 + mid32*2^32 + lo32) / 10^scale  (with sign)
        uint32_t fields[4];
        memcpy(fields, val, sizeof(fields));
        uint32_t lo32   = fields[0];
        uint32_t mid32  = fields[1];
        uint32_t hi32   = fields[2];
        uint32_t flags  = fields[3];
        uint32_t scale  = (flags >> 16) & 0xFF;
        int      negative = (flags >> 31) & 1;

        // Reconstruct 96-bit mantissa as decimal string.
        // Use __uint128_t for the arithmetic.
        __uint128_t m = ((__uint128_t)hi32 << 64) |
                        ((__uint128_t)mid32 << 32) |
                        (__uint128_t)lo32;

        if (m == 0) {
            bz_jser_cstr(s, "0");
            bz_jser_mark_comma(s);
            return;
        }

        // Convert mantissa to decimal string (up to 29 digits + NUL).
        char digits[40] = {};
        int  ndigits = 0;
        __uint128_t tmp = m;
        while (tmp > 0) {
            digits[ndigits++] = (char)('0' + (int)(tmp % 10));
            tmp /= 10;
        }
        // digits is in reverse order; flip it.
        for (int a = 0, b = ndigits - 1; a < b; ++a, --b) {
            char t = digits[a]; digits[a] = digits[b]; digits[b] = t;
        }

        // Build the JSON number: [-] integer-part [. fraction-part]
        char out[64];
        int  olen = 0;
        if (negative) out[olen++] = '-';

        if (scale == 0) {
            // Pure integer.
            for (int i = 0; i < ndigits; ++i) out[olen++] = digits[i];
        } else {
            // ndigits with scale decimal places.
            int int_digits = ndigits - (int)scale;
            if (int_digits > 0) {
                for (int i = 0; i < int_digits; ++i) out[olen++] = digits[i];
                out[olen++] = '.';
                for (int i = int_digits; i < ndigits; ++i) out[olen++] = digits[i];
            } else {
                // Number is 0.000...digits (int_digits <= 0).
                out[olen++] = '0';
                out[olen++] = '.';
                int leading_zeros = -int_digits;
                for (int i = 0; i < leading_zeros; ++i) out[olen++] = '0';
                for (int i = 0; i < ndigits; ++i) out[olen++] = digits[i];
            }
        }
        bz_jser_raw(s, out, olen);
        bz_jser_mark_comma(s);
        return;
    }
    if (!val) {
        uint32_t zero = 0;
        for (int i = 0; i < 4; ++i) bz_binser_write_le32_impl(s, zero);
        return;
    }
    uint32_t fields[4];
    memcpy(fields, val, sizeof(fields));
    for (int i = 0; i < 4; ++i)
        bz_binser_write_le32_impl(s, fields[i]);
}

void bz_binser_write_bool(void* serp, int8_t v) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        bz_jser_maybe_comma(s);
        bz_jser_cstr(s, v ? "true" : "false");
        bz_jser_mark_comma(s);
        return;
    }
    bz_binser_write_byte_impl(s, v ? 1 : 0);
}

void bz_binser_write_string(void* serp, void* strp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        bz_jser_maybe_comma(s);
        bz_jser_write_string_impl(s, strp);
        bz_jser_mark_comma(s);
        return;
    }
    if (!strp) {
        bz_binser_write_le32_impl(s, 0);
        return;
    }
    const auto* str = static_cast<const bz_string_struct*>(strp);
    if (str->width == 1) {
        bz_binser_write_utf8_bytes(s,
            static_cast<const char*>(str->data), str->len);
    } else {
        char* utf8 = bz_string_to_utf8z(strp);
        int64_t n = utf8 ? (int64_t)strlen(utf8) : 0;
        bz_binser_write_utf8_bytes(s, utf8 ? utf8 : "", n);
        free(utf8);
    }
}

void bz_binser_write_char(void* serp, int32_t codepoint) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        // §2.15: WriteChar → one-character JSON string.
        // Synthesize a tiny bz_string_struct on the stack, reuse write_string_impl.
        uint8_t  tmp_data[5] = {};
        int64_t  tmp_len  = 1;
        uint8_t  tmp_width;
        uint32_t cp = (uint32_t)codepoint;
        if (cp <= 0xFF)       { tmp_data[0] = (uint8_t)cp; tmp_width = 1; }
        else if (cp <= 0xFFFF){ uint16_t v = (uint16_t)cp; memcpy(tmp_data, &v, 2); tmp_width = 2; }
        else                  { memcpy(tmp_data, &cp, 4); tmp_width = 4; }
        // Build a minimal bz_string_struct on the stack.
        uint8_t str_bytes[BZ_STRING_SIZEOF] = {};
        void* dp = tmp_data;
        memcpy(str_bytes,      &dp,       sizeof(void*));
        memcpy(str_bytes + 8,  &tmp_len,  sizeof(int64_t));
        str_bytes[16] = tmp_width;
        bz_jser_maybe_comma(s);
        bz_jser_write_string_impl(s, str_bytes);
        bz_jser_mark_comma(s);
        return;
    }
    char buf[4];
    int n = 0;
    uint32_t cp = (uint32_t)codepoint;
    if      (cp < 0x80)     { buf[n++] = (char)cp; }
    else if (cp < 0x800)    { buf[n++] = (char)(0xC0|(cp>>6));  buf[n++] = (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000)  { buf[n++] = (char)(0xE0|(cp>>12)); buf[n++] = (char)(0x80|((cp>>6)&0x3F));  buf[n++] = (char)(0x80|(cp&0x3F)); }
    else                    { buf[n++] = (char)(0xF0|(cp>>18)); buf[n++] = (char)(0x80|((cp>>12)&0x3F)); buf[n++] = (char)(0x80|((cp>>6)&0x3F)); buf[n++] = (char)(0x80|(cp&0x3F)); }
    bz_binser_write_utf8_bytes(s, buf, n);
}

void bz_binser_write_null(void* serp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        bz_jser_maybe_comma(s);
        bz_jser_cstr(s, "null");
        bz_jser_mark_comma(s);
        return;
    }
    bz_binser_write_byte_impl(s, 0);
}

void bz_binser_begin_object(void* serp, int32_t field_count) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        // §2.15: fieldCount is advisory for JSON — ignored.
        (void)field_count;
        // fix #687 / stdlib §2.15 MaxDepth: raise SerializationException when
        // nesting exceeds the configured max_depth.
        if (s->json.max_depth > 0 && s->json.depth >= s->json.max_depth) {
            bz_ser_throw("JSON nesting exceeds MaxDepth (stdlib §2.15, #687)", nullptr);
            return;
        }
        bz_jser_maybe_comma(s);
        bz_jser_cstr(s, "{");
        int nd = ++s->json.depth;
        if (nd < BZ_JSER_MAX_DEPTH) s->json.needs_comma[nd] = false;
        // §2.15 v0.139: if write_inline_tag left a pending tag, inject
        // `"$id":n` as the first (virtual) field inside this object.
        if (s->json_pending_tag_id > 0) {
            // fix #687: in indented mode add indent before the $id field too.
            if (s->json.indented) bz_jser_indent(s);
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\"$id\":%d", s->json_pending_tag_id);
            bz_jser_raw(s, buf, n);
            if (nd < BZ_JSER_MAX_DEPTH) s->json.needs_comma[nd] = true;
            s->json_pending_tag_id = 0;
        }
        return;
    }
    bz_binser_write_le32_impl(s, (uint32_t)field_count);
}

// write_field_name: name is a bz_string* (void*)
void bz_binser_write_field_name(void* serp, void* name) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        // For JSON: write comma+quote+field_name+":".
        // The comma logic: between fields in an object.
        int d = s->json.depth;
        if (d >= 0 && d < BZ_JSER_MAX_DEPTH && s->json.needs_comma[d]) {
            bz_jser_cstr(s, ",");
        }
        // fix #687: in indented mode, emit newline+indent before each field name.
        if (s->json.indented) {
            bz_jser_indent(s);
        }
        bz_jser_write_string_impl(s, name);
        if (s->json.indented) {
            bz_jser_cstr(s, ": ");  // space after colon for readability
        } else {
            bz_jser_cstr(s, ":");
        }
        // Reset the needs_comma flag (the value that follows is NOT preceded by comma).
        if (d >= 0 && d < BZ_JSER_MAX_DEPTH) s->json.needs_comma[d] = false;
        // fix #687: signal that the next maybe_comma call should skip the indent
        // (the value comes right after ": " on the same line).
        s->json.after_field_name = true;
        return;
    }
    bz_binser_write_string(serp, name);
}

void bz_binser_end_object(void* serp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        int d = s->json.depth;
        --s->json.depth;
        // fix #687: in indented mode, emit newline+indent before closing brace.
        if (s->json.indented && d > 0) {
            // Only emit newline if there were any fields (needs_comma[d] is true
            // if at least one field was written).
            if (d >= 0 && d < BZ_JSER_MAX_DEPTH && s->json.needs_comma[d]) {
                bz_jser_indent(s);
            }
        }
        bz_jser_cstr(s, "}");
        // After the } the parent context needs a comma.
        bz_jser_mark_comma(s);
        return;
    }
    /* no trailer in binary format */
}

void bz_binser_begin_array(void* serp, int32_t element_count) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        (void)element_count;
        bz_jser_maybe_comma(s);
        bz_jser_cstr(s, "[");
        int nd = ++s->json.depth;
        if (nd < BZ_JSER_MAX_DEPTH) s->json.needs_comma[nd] = false;
        return;
    }
    bz_binser_write_le32_impl(s, (uint32_t)element_count);
}

void bz_binser_end_array(void* serp) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        int d = s->json.depth;
        --s->json.depth;
        // fix #687: in indented mode, emit newline+indent before closing bracket.
        if (s->json.indented && d > 0 &&
            d >= 0 && d < BZ_JSER_MAX_DEPTH && s->json.needs_comma[d]) {
            bz_jser_indent(s);
        }
        bz_jser_cstr(s, "]");
        bz_jser_mark_comma(s);
        return;
    }
    /* no trailer */
}

// ---------------------------------------------------------------------------
// §9.8 tier-3: identity-tag graph serialization helpers
// ---------------------------------------------------------------------------

// Return the tag_id already assigned to `ptr`, or 0 if not yet seen.
// Per §9.8: a tag_id of 0 means "first reach through an owning edge" (must be
// written inline); non-zero means "already written" (must be written as BZ_SER_REF).
int32_t bz_binser_graph_get_tag(void* serp, void* ptr) {
    if (!serp || !ptr) return 0;
    auto* s = static_cast<bz_binser_t*>(serp);
    for (int32_t i = 0; i < s->tag_count; ++i)
        if (s->tag_ptrs[i] == ptr)
            return i + 1;  // tag_id is 1-based
    return 0;
}

// Assign a new tag_id to `ptr` and return it.
// Called for the first owning reach of each object in a `serialized graph`.
int32_t bz_binser_graph_assign_tag(void* serp, void* ptr) {
    if (!serp || !ptr) return 0;
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->tag_count >= s->tag_cap) {
        int32_t newcap = s->tag_cap == 0 ? 16 : s->tag_cap * 2;
        // fix #1762 finding R5: same realloc anti-pattern as bz_binser_ensure,
        // same §31.4 posture. Returning 0 here would mean "not seen before" to
        // every caller, silently breaking §9.8 graph identity (the same object
        // would be re-emitted instead of back-referenced) rather than failing.
        void* tmp = realloc(s->tag_ptrs, (size_t)newcap * sizeof(void*));
        if (!tmp) bz_alloc_fail("serializer graph identity-tag table growth");
        s->tag_ptrs = static_cast<void**>(tmp);
        s->tag_cap  = newcap;
    }
    s->tag_ptrs[s->tag_count++] = ptr;
    return s->tag_count;  // 1-based tag_id
}

// Write BZ_SER_INLINE (0x01) + LE32 tag_id.  Precedes the inline object body.
// §2.15 v0.139 JSON form: emit `"$id":n,` as the first field inside the
// already-opened object (BeginObject was called just before this).
// The JSON depth/comma state is already pushed; we write the pseudo-field
// and set needs_comma so the first real field gets a comma.
void bz_binser_write_inline_tag(void* serp, int32_t tag_id) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (s->kind == 1) {
        // §2.15 v0.139 JSON: defer emission of `"$id":n` until begin_object is
        // called (which writes `{` first).  Storing here; begin_object injects it
        // as the first virtual field.  Writing here would precede `{` and produce
        // malformed JSON.
        s->json_pending_tag_id = tag_id;
        return;
    }
    bz_binser_write_byte_impl(s, BZ_SER_INLINE);
    bz_binser_write_le32_impl(s, (uint32_t)tag_id);
}

// Write BZ_SER_REF (0x02) + LE32 tag_id.  Used for back-edge / aliased references.
// §2.15 v0.139 JSON form: emit `{"$ref":n}` as a complete object value in place
// of the back-reference.  This is written at value position (after a field name
// or in an array), so we must handle the surrounding comma state.
//
// §9.8 cycle detection: tag_id=0 means the back-edge target was never registered
// as an owned component — it is the root object or an external object that forms
// a cycle.  Raise SerializationException instead of emitting a dangling $ref.
void bz_binser_write_ref_tag(void* serp, int32_t tag_id) {
    auto* s = static_cast<bz_binser_t*>(serp);
    if (tag_id <= 0) {
        bz_ser_throw("Cycle detected in serialized graph/cyclic object (§9.8)", nullptr);
        return;
    }
    if (s->kind == 1) {
        // JSON: {"$ref":n}
        // bz_jser_maybe_comma writes comma if needed at current depth;
        // bz_jser_mark_comma tells the parent a value was written.
        bz_jser_maybe_comma(s);
        char buf[40];
        int n = snprintf(buf, sizeof(buf), "{\"$ref\":%d}", tag_id);
        bz_jser_raw(s, buf, n);
        bz_jser_mark_comma(s);
        return;
    }
    bz_binser_write_byte_impl(s, BZ_SER_REF);
    bz_binser_write_le32_impl(s, (uint32_t)tag_id);
}

} // extern "C" (writer half)

// --- reader state -----------------------------------------------------------

// JSON deserializer state (used when kind == 1).
// The parser is a simple recursive-descent tokenizer.
// We track the nesting stack for the object/array depth counter.
#define BZ_JDER_MAX_DEPTH 64

struct bz_binder_t {
    const uint8_t* data;
    int64_t        len;
    int64_t        pos;
    int8_t         ignore_unknown;
    int32_t        fields_remaining;
    bool           in_object;
    int8_t         kind;  // 0 = binary, 1 = JSON, 2 = JSON+owns data
    // fix #687 / stdlib §2.15 JsonOptions:
    int8_t         allow_trailing_commas;  // tolerate trailing comma in JSON objects/arrays
    int32_t        max_depth;              // nesting guard (0 = unlimited)

    // §2.15 v0.139 JSON graph-identity: set to 1 when bz_binder_read_graph_marker
    // has already consumed the opening `{` of an inline object (and the `"$id":n,`
    // pseudo-field).  bz_binder_begin_object checks this flag and, when set,
    // skips re-reading `{` and starts the field count from the current position.
    int8_t  json_obj_preread;

    // fix #688 / stdlib §2.15: JSON position tracking — line/column for error messages.
    // Line and column are 1-based; updated by bz_jder_advance_tracking as characters
    // are consumed.  Only meaningful when kind >= 1 (JSON).
    int64_t json_line;  // current line (1-based)
    int64_t json_col;   // current column (1-based)

    // §9.8 tier-3: identity-tag→object table for `serialized graph` deserialize.
    // tag_ptrs[i]        = the constructed object pointer for tag_id (i+1).
    //                      NULL while the object is still in-progress (cycle marker).
    // tag_in_progress[i] = 1 while tag (i+1) is currently being constructed.
    // The table is allocated on first use.
    void**   tag_ptrs;
    int8_t*  tag_in_progress;
    int32_t  tag_count;  // slots allocated (capacity = tag_count; grows as needed)

    // §9.8 tier-4: polymorphic dispatch — preread field count.
    // Set by bz_binder_read_poly_discriminator after consuming the `$type` field;
    // the next bz_binder_begin_object call returns this value instead of reading
    // from the stream (the opening '{' and `$type` field have already been consumed).
    // For JSON: json_obj_preread is also set to 1 alongside this.
    int32_t  poly_preread_count;  // preread field count when poly_preread_active
    // §9.8 choice serialization (v0.200, #2046): a payload-free choice case has
    // ZERO remaining fields, so `poly_preread_count > 0` cannot gate the preread
    // (0 is indistinguishable from "unset").  This companion flag makes a preread
    // of 0 honoured: when set, the next bz_binder_begin_object returns
    // poly_preread_count (possibly 0) without re-reading the object header.
    int8_t   poly_preread_active;  // 0 = unset; 1 = preread_count is authoritative
};

static int bz_binder_read_byte_impl(bz_binder_t* d, uint8_t* out) {
    if (d->pos >= d->len) return 0;
    *out = d->data[d->pos++];
    return 1;
}

static int bz_binder_read_le32_impl(bz_binder_t* d, uint32_t* out) {
    if (d->pos + 4 > d->len) return 0;
    *out = ((uint32_t)d->data[d->pos+0])
         | ((uint32_t)d->data[d->pos+1] <<  8)
         | ((uint32_t)d->data[d->pos+2] << 16)
         | ((uint32_t)d->data[d->pos+3] << 24);
    d->pos += 4;
    return 1;
}

static int bz_binder_read_le64_impl(bz_binder_t* d, uint64_t* out) {
    if (d->pos + 8 > d->len) return 0;
    *out = 0;
    for (int i = 0; i < 8; ++i)
        *out |= ((uint64_t)d->data[d->pos+i]) << (8*i);
    d->pos += 8;
    return 1;
}

// fix #688 / §2.15: forward-declared here so bz_binder_read_bz_string (below,
// used before the definition at its usual spot near the other throw helpers)
// can raise on binary truncation the same way bz_binder_read_int/_uint/_float
// already do. Defined further down in this file.
static void bz_ser_throw_at_binary(const char* msg, int64_t offset);

// Read a LE32-prefixed UTF-8 byte string and return a heap-allocated
// bz_string_struct* with adaptive-width code-point storage.
// Raises SerializationException (bz_ser_throw_at_binary) on truncated data —
// fix #2403 / stdlib §2.14/§2.15 (v0.235): a truncated length prefix or a
// length prefix whose declared byte count exceeds the remaining buffer is
// "malformed input" under the stdlib's Errors rule ("Malformed input ...
// raise SerializationException"; §2.15 also states the binary format "is
// length-prefixed", so an unsatisfiable length prefix is definitionally
// malformed) — exactly the same failure shape bz_binder_read_int/_uint/_float
// already raise on. Previously this returned nullptr silently, so a truncated
// string VALUE (via bz_binder_read_string), CHAR (bz_binder_read_char), or
// FIELD NAME/discriminator (the other callers below) looked like ordinary
// end-of-data instead of an error — the "worse than a leak" silent-data-loss
// defect in #2403, sharing its root cause with #2394's bool/decimal reports
// (see the fixes to bz_binder_read_bool/bz_binder_read_decimal below).
//
// #635: the bz_string `len` field is a CODE-POINT count, not a byte count.
// Previously this function set len=byte_len with width=1 (treating each byte as
// one code point).  For multi-byte UTF-8 strings (CJK, emoji, etc.) this created
// a length mismatch against the original string literal whose len=code_point_count;
// emitStringEq's length check then always returned false even for identical content.
//
// Fix: decode the UTF-8 bytes into code points, select the narrowest width that
// fits all code points (1/2/4, per §4.6 adaptive-width storage), allocate the data
// buffer at that width, and set len to the decoded code-point count.  This mirrors
// exactly what lowerStringLiteralConst() does at compile time.
static void* bz_binder_read_bz_string(bz_binder_t* d) {
    // fix #2403 / §2.15: capture the start-of-value offset once, mirroring the
    // scalar readers and the object-header read above — both failure sites
    // below (truncated length prefix, truncated payload) report the same
    // start-of-field offset rather than the mid-read position.
    int64_t off = d->pos;
    uint32_t byte_len = 0;
    if (!bz_binder_read_le32_impl(d, &byte_len)) {
        bz_ser_throw_at_binary("Truncated binary input reading string length (§2.15)", off);
        return nullptr;
    }
    if (d->pos + (int64_t)byte_len > d->len) {
        bz_ser_throw_at_binary("Truncated binary input reading string data (§2.15)", off);
        return nullptr;
    }

    const uint8_t* src = d->data + d->pos;
    d->pos += (int64_t)byte_len;

    // --- pass 1: decode UTF-8 bytes → code points; determine count + max cp ---
    // Worst case: every byte is a single ASCII code point.
    uint32_t* cps = static_cast<uint32_t*>(malloc((size_t)byte_len * sizeof(uint32_t) + 4));
    if (!cps) return nullptr;
    int64_t ncp = 0;
    uint32_t max_cp = 0;
    int64_t i = 0;
    while (i < (int64_t)byte_len) {
        uint8_t b0 = src[i];
        uint32_t cp;
        int seq;
        if      (b0 < 0x80) { cp = b0;                    seq = 1; }
        else if (b0 < 0xC0) { cp = 0xFFFD; seq = 1; }  // unexpected continuation → replacement
        else if (b0 < 0xE0) { cp = b0 & 0x1F;            seq = 2; }
        else if (b0 < 0xF0) { cp = b0 & 0x0F;            seq = 3; }
        else                 { cp = b0 & 0x07;            seq = 4; }
        for (int k = 1; k < seq && i + k < (int64_t)byte_len; ++k)
            cp = (cp << 6) | (src[i + k] & 0x3F);
        i += seq;
        cps[ncp++] = cp;
        if (cp > max_cp) max_cp = cp;
    }

    // --- pass 2: select adaptive width (§4.6) ---
    uint8_t width = (max_cp <= 0xFF) ? 1 : (max_cp <= 0xFFFF) ? 2 : 4;

    // --- pass 3: allocate data buffer at selected width ---
    size_t data_bytes = (size_t)ncp * width + width; // +1 element NUL terminator
    uint8_t* buf = static_cast<uint8_t*>(malloc(data_bytes));
    if (!buf) { free(cps); return nullptr; }
    memset(buf + (size_t)ncp * width, 0, width); // NUL terminator
    for (int64_t j = 0; j < ncp; ++j) {
        if (width == 1)
            buf[j] = (uint8_t)cps[j];
        else if (width == 2) {
            uint16_t v = (uint16_t)cps[j];
            memcpy(buf + j * 2, &v, 2);
        } else {
            uint32_t v = cps[j];
            memcpy(buf + j * 4, &v, 4);
        }
    }
    free(cps);

    // --- build the bz_string header ---
    // Allocate a full BZ_STRING_SIZEOF (32-byte) object with rc initialized so
    // that bz_string_release works correctly (#550: deserialized field names are
    // passed to bz_string_release by generated code).
    uint8_t* strObj = static_cast<uint8_t*>(calloc(1, (size_t)BZ_STRING_SIZEOF));
    if (!strObj) { free(buf); return nullptr; }
    void* buf_ptr = buf;
    memcpy(strObj,      &buf_ptr, sizeof(void*));   // data
    memcpy(strObj + 8,  &ncp,     sizeof(int64_t)); // len = CODE-POINT count
    strObj[16] = width;                              // width
    bz_string_init_rc(strObj);  // #422: rc := 1 (producer's reference)
    return strObj;
}

// Forward declarations needed by JSON helpers.
// (bz_decimal_t is defined in bezel_runtime.h — already included above)
extern "C" void bz_string_release(void* s);
// bz_decimal_parse_cstr: (text, out, ove_vtable, fe_vtable) — stores result in *out.
extern "C" void bz_decimal_parse_cstr(const char* s, bz_decimal_t* out,
                                       void* ove_vtable, void* fe_vtable);

// ---------------------------------------------------------------------------
// JSON deserializer helpers (kind == 1)
//
// §2.15 JSON read rules:
//   BeginObject → skip '{', return -1
//   ReadFieldName → read key string (unquote+unescape), skip ':'
//   SkipValue → skip one JSON value (any type, including nested {}/[])
//   EndObject → skip '}'
//   BeginArray → skip '[', return -1
//   HasMoreElements → peek: return 1 unless next non-ws char is ']'
//   EndArray → skip ']'
//   ReadInt/ReadUInt → parse JSON number as integer
//   ReadFloat → parse JSON number as double
//   ReadDecimal → parse JSON number as decimal
//   ReadBool → parse true/false
//   ReadString → parse JSON string value
//   ReadChar → parse JSON string, take first codepoint
//   TryReadNull → if next non-ws is "null", consume and return 1, else 0
//
// All read functions throw SerializationException on malformed input.
// ---------------------------------------------------------------------------

// Skip whitespace; track newlines for line/column position (#688).
static void bz_jder_skip_ws(bz_binder_t* d) {
    while (d->pos < d->len) {
        uint8_t c = d->data[d->pos];
        if (c == '\n') {
            ++d->pos; ++d->json_line; d->json_col = 1;
        } else if (c == '\r') {
            ++d->pos;
            // CR-LF: absorb the LF together with the CR
            if (d->pos < d->len && d->data[d->pos] == '\n') ++d->pos;
            ++d->json_line; d->json_col = 1;
        } else if (c == ' ' || c == '\t') {
            ++d->pos; ++d->json_col;
        } else {
            break;
        }
    }
}

// Peek next non-ws character (does NOT advance pos).
// Returns -1 on end of input.
static int bz_jder_peek(bz_binder_t* d) {
    bz_jder_skip_ws(d);
    if (d->pos >= d->len) return -1;
    return d->data[d->pos];
}

// fix #688 / stdlib §2.15: compute the 1-based line+column at `pos` by scanning
// from the beginning of the input.  Called only on error paths.
static void bz_jder_line_col_at(const bz_binder_t* d, int64_t pos,
                                 int64_t* out_line, int64_t* out_col) {
    int64_t line = 1, col = 1;
    for (int64_t i = 0; i < pos && i < d->len; ) {
        uint8_t c = d->data[i++];
        if (c == '\n') { ++line; col = 1; }
        else if (c == '\r') {
            ++line; col = 1;
            if (i < d->len && d->data[i] == '\n') ++i; // skip LF of CRLF
        } else {
            ++col;
        }
    }
    *out_line = line;
    *out_col  = col;
}

// fix #688 / stdlib §2.15: build a positioned message and throw.
// For JSON: appends " at line L, column C".
// For binary: appends " at byte offset N".
// bz_ser_throw_at_json uses bz_jder_line_col_at so it is always accurate even
// when the error is detected mid-token (not after a skip_ws call).
static void bz_ser_throw_at_json(const char* msg, const bz_binder_t* d) {
    int64_t line = 1, col = 1;
    // Use current d->pos as the fault position.
    bz_jder_line_col_at(d, d->pos, &line, &col);
    // Format: "<msg> at line L, column C"
    char pos_buf[64];
    int n = snprintf(pos_buf, sizeof(pos_buf), " at line %lld, column %lld",
                     (long long)line, (long long)col);
    (void)n;
    size_t msg_len = msg ? strlen(msg) : 0;
    size_t pos_len = strlen(pos_buf);
    char* buf = static_cast<char*>(malloc(msg_len + pos_len + 1));
    if (msg_len > 0) memcpy(buf, msg, msg_len);
    memcpy(buf + msg_len, pos_buf, pos_len);
    buf[msg_len + pos_len] = '\0';
    bz_ser_throw(buf, nullptr);
    free(buf);
}

// fix #688 / stdlib §2.15: binary raise with byte-offset annotation.
static void bz_ser_throw_at_binary(const char* msg, int64_t offset) {
    char pos_buf[64];
    int n = snprintf(pos_buf, sizeof(pos_buf), " at byte offset %lld", (long long)offset);
    (void)n;
    size_t msg_len = msg ? strlen(msg) : 0;
    size_t pos_len = strlen(pos_buf);
    char* buf = static_cast<char*>(malloc(msg_len + pos_len + 1));
    if (msg_len > 0) memcpy(buf, msg, msg_len);
    memcpy(buf + msg_len, pos_buf, pos_len);
    buf[msg_len + pos_len] = '\0';
    bz_ser_throw(buf, nullptr);
    free(buf);
}

// Throw a SerializationException with position and return.
#define BZ_JDER_THROW(d, msg) do { \
    bz_ser_throw_at_json((msg), (d)); \
    return; \
} while(0)
#define BZ_JDER_THROW_V(d, msg, ret) do { \
    bz_ser_throw_at_json((msg), (d)); \
    return (ret); \
} while(0)

// Expect a specific character, skip ws first, advance pos.
// Returns true on success, throws otherwise.
static bool bz_jder_expect_char(bz_binder_t* d, char expected) {
    bz_jder_skip_ws(d);
    if (d->pos >= d->len) {
        bz_ser_throw_at_json("Unexpected end of JSON input (§2.15)", d);
        return false;
    }
    if ((char)d->data[d->pos] != expected) {
        bz_ser_throw_at_json("Unexpected JSON token (§2.15)", d);
        return false;
    }
    ++d->pos;
    return true;
}

// Decode a single UTF-8 codepoint from src[i..]; advance i.
// Returns the codepoint.
static uint32_t bz_jder_decode_utf8(const uint8_t* src, int64_t len, int64_t* i) {
    uint8_t b0 = src[*i];
    uint32_t cp;
    int seq;
    if      (b0 < 0x80) { cp = b0;        seq = 1; }
    else if (b0 < 0xC0) { cp = 0xFFFD;    seq = 1; }
    else if (b0 < 0xE0) { cp = b0 & 0x1F; seq = 2; }
    else if (b0 < 0xF0) { cp = b0 & 0x0F; seq = 3; }
    else                 { cp = b0 & 0x07; seq = 4; }
    for (int k = 1; k < seq && *i + k < len; ++k)
        cp = (cp << 6) | (src[*i + k] & 0x3F);
    *i += seq;
    return cp;
}

// Parse a JSON string value at current pos (which should be at '"').
// Returns a malloc'd array of uint32_t codepoints.
// *ncp_out receives the code-point count.
// Returns nullptr on malformed input (and has thrown).
static uint32_t* bz_jder_parse_string_cps(bz_binder_t* d, int64_t* ncp_out) {
    if (d->pos >= d->len || d->data[d->pos] != '"') {
        bz_ser_throw_at_json("Expected '\"' in JSON string (§2.15)", d);
        *ncp_out = 0;
        return nullptr;
    }
    ++d->pos;  // skip opening "

    // Collect codepoints into a growable buffer.
    int64_t cap = 32;
    int64_t n   = 0;
    uint32_t* cps = static_cast<uint32_t*>(malloc((size_t)cap * sizeof(uint32_t)));
    if (!cps) { bz_ser_throw_at_json("OOM in JSON string parse", d); *ncp_out=0; return nullptr; }

    while (d->pos < d->len) {
        uint8_t b = d->data[d->pos];
        if (b == '"') { ++d->pos; break; }  // end of string

        uint32_t cp;
        if (b == '\\') {
            ++d->pos;
            if (d->pos >= d->len) { free(cps); bz_ser_throw_at_json("Truncated JSON escape (§2.15)", d); *ncp_out=0; return nullptr; }
            uint8_t esc = d->data[d->pos++];
            switch (esc) {
            case '"':  cp = '"';  break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/';  break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'u': {
                // \uXXXX — read 4 hex digits.
                if (d->pos + 4 > d->len) { free(cps); bz_ser_throw_at_json("Short \\uXXXX escape (§2.15)", d); *ncp_out=0; return nullptr; }
                uint32_t code = 0;
                for (int k = 0; k < 4; ++k) {
                    uint8_t hx = d->data[d->pos+k];
                    uint32_t nib;
                    if (hx >= '0' && hx <= '9')      nib = hx - '0';
                    else if (hx >= 'a' && hx <= 'f') nib = hx - 'a' + 10;
                    else if (hx >= 'A' && hx <= 'F') nib = hx - 'A' + 10;
                    else { free(cps); bz_ser_throw_at_json("Bad hex in \\uXXXX (§2.15)", d); *ncp_out=0; return nullptr; }
                    code = (code << 4) | nib;
                }
                d->pos += 4;
                cp = code;
                break;
            }
            default:
                free(cps); bz_ser_throw_at_json("Unknown JSON escape (§2.15)", d); *ncp_out=0; return nullptr;
            }
        } else {
            // Raw UTF-8 codepoint.
            cp = bz_jder_decode_utf8(d->data, d->len, &d->pos);
        }

        if (n >= cap) {
            cap *= 2;
            uint32_t* nc = static_cast<uint32_t*>(realloc(cps, (size_t)cap * sizeof(uint32_t)));
            if (!nc) { free(cps); bz_ser_throw_at_json("OOM in JSON string parse", d); *ncp_out=0; return nullptr; }
            cps = nc;
        }
        cps[n++] = cp;
    }
    *ncp_out = n;
    return cps;
}

// Convert a codepoint array to a bz_string_struct*.
static void* bz_jder_cps_to_bz_string(const uint32_t* cps, int64_t ncp) {
    uint32_t max_cp = 0;
    for (int64_t i = 0; i < ncp; ++i) if (cps[i] > max_cp) max_cp = cps[i];
    uint8_t width = (max_cp <= 0xFF) ? 1 : (max_cp <= 0xFFFF) ? 2 : 4;
    size_t data_bytes = (size_t)ncp * width + width;
    uint8_t* buf = static_cast<uint8_t*>(malloc(data_bytes));
    if (!buf) return nullptr;
    memset(buf + (size_t)ncp * width, 0, width);
    for (int64_t j = 0; j < ncp; ++j) {
        if (width == 1)       buf[j]      = (uint8_t)cps[j];
        else if (width == 2) { uint16_t v = (uint16_t)cps[j]; memcpy(buf + j*2, &v, 2); }
        else                 { uint32_t v = cps[j];            memcpy(buf + j*4, &v, 4); }
    }
    uint8_t* strObj = static_cast<uint8_t*>(calloc(1, (size_t)BZ_STRING_SIZEOF));
    if (!strObj) { free(buf); return nullptr; }
    void* bufp = buf;
    memcpy(strObj,      &bufp, sizeof(void*));
    memcpy(strObj + 8,  &ncp,  sizeof(int64_t));
    strObj[16] = width;
    bz_string_init_rc(strObj);
    return strObj;
}

// Parse a JSON string and return a bz_string_struct*.
static void* bz_jder_parse_string(bz_binder_t* d) {
    int64_t ncp = 0;
    uint32_t* cps = bz_jder_parse_string_cps(d, &ncp);
    if (!cps) return nullptr;
    void* str = bz_jder_cps_to_bz_string(cps, ncp);
    free(cps);
    return str;
}

// Skip one complete JSON value at current pos (after ws skip).
// Used by SkipValue (unknown-field policy).
static void bz_jder_skip_value(bz_binder_t* d);

static void bz_jder_skip_value_impl(bz_binder_t* d) {
    int c = bz_jder_peek(d);
    if (c < 0) { bz_ser_throw_at_json("Unexpected end in JSON skip (§2.15)", d); return; }

    if (c == '"') {
        // String: parse and discard.
        int64_t ncp = 0;
        uint32_t* cps = bz_jder_parse_string_cps(d, &ncp);
        free(cps);
    } else if (c == '{') {
        ++d->pos;  // skip '{'
        bz_jder_skip_ws(d);
        if (d->pos < d->len && d->data[d->pos] == '}') { ++d->pos; return; }
        while (true) {
            bz_jder_skip_ws(d);
            // key
            int64_t ncp = 0;
            uint32_t* cps = bz_jder_parse_string_cps(d, &ncp);
            free(cps);
            if (!bz_jder_expect_char(d, ':')) return;
            bz_jder_skip_value(d);
            bz_jder_skip_ws(d);
            if (d->pos >= d->len) { bz_ser_throw_at_json("Truncated JSON object in skip (§2.15)", d); return; }
            if (d->data[d->pos] == ',') { ++d->pos; continue; }
            if (d->data[d->pos] == '}') { ++d->pos; return; }
            bz_ser_throw_at_json("Expected ',' or '}' in JSON object skip (§2.15)", d);
            return;
        }
    } else if (c == '[') {
        ++d->pos;  // skip '['
        bz_jder_skip_ws(d);
        if (d->pos < d->len && d->data[d->pos] == ']') { ++d->pos; return; }
        while (true) {
            bz_jder_skip_value(d);
            bz_jder_skip_ws(d);
            if (d->pos >= d->len) { bz_ser_throw_at_json("Truncated JSON array in skip (§2.15)", d); return; }
            if (d->data[d->pos] == ',') { ++d->pos; continue; }
            if (d->data[d->pos] == ']') { ++d->pos; return; }
            bz_ser_throw_at_json("Expected ',' or ']' in JSON array skip (§2.15)", d);
            return;
        }
    } else if (c == 't' || c == 'f' || c == 'n') {
        // true / false / null
        const uint8_t* kw = d->data + d->pos;
        if (c == 't' && d->pos + 4 <= d->len && memcmp(kw, "true",  4) == 0) d->pos += 4;
        else if (c == 'f' && d->pos + 5 <= d->len && memcmp(kw, "false", 5) == 0) d->pos += 5;
        else if (c == 'n' && d->pos + 4 <= d->len && memcmp(kw, "null",  4) == 0) d->pos += 4;
        else { bz_ser_throw_at_json("Unknown JSON keyword in skip (§2.15)", d); }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        // number: consume digits and optional .digits and optional eXX
        if (d->data[d->pos] == '-') ++d->pos;
        while (d->pos < d->len && (d->data[d->pos] >= '0' && d->data[d->pos] <= '9')) ++d->pos;
        if (d->pos < d->len && d->data[d->pos] == '.') {
            ++d->pos;
            while (d->pos < d->len && (d->data[d->pos] >= '0' && d->data[d->pos] <= '9')) ++d->pos;
        }
        if (d->pos < d->len && (d->data[d->pos] == 'e' || d->data[d->pos] == 'E')) {
            ++d->pos;
            if (d->pos < d->len && (d->data[d->pos] == '+' || d->data[d->pos] == '-')) ++d->pos;
            while (d->pos < d->len && (d->data[d->pos] >= '0' && d->data[d->pos] <= '9')) ++d->pos;
        }
    } else {
        bz_ser_throw_at_json("Unexpected JSON token in skip (§2.15)", d);
    }
}

static void bz_jder_skip_value(bz_binder_t* d) {
    bz_jder_skip_value_impl(d);
}

// Parse a JSON number as a 64-bit integer.
// Advances pos past the number. Throws on non-number token.
static int64_t bz_jder_parse_int(bz_binder_t* d) {
    bz_jder_skip_ws(d);
    if (d->pos >= d->len) {
        bz_ser_throw_at_json("Expected JSON number, got end of input (§2.15)", d);
        return 0;
    }
    // Collect the number text.
    int64_t start = d->pos;
    if (d->data[d->pos] == '-') ++d->pos;
    if (d->pos >= d->len || d->data[d->pos] < '0' || d->data[d->pos] > '9') {
        bz_ser_throw_at_json("Expected JSON integer (§2.15)", d);
        return 0;
    }
    while (d->pos < d->len && d->data[d->pos] >= '0' && d->data[d->pos] <= '9') ++d->pos;
    // §2.15 (v0.314, fix #5400): no decimal point or exponent allowed for integer
    // reads — "5.9" must raise SerializationException, not silently truncate to 5.
    if (d->pos < d->len && (d->data[d->pos] == '.' || d->data[d->pos] == 'e' || d->data[d->pos] == 'E')) {
        bz_ser_throw_at_json("JSON integer cannot have fraction or exponent (§2.15)", d);
        return 0;
    }
    int64_t numlen = d->pos - start;
    if (numlen > 20) { bz_ser_throw_at_json("JSON integer overflow (§2.15)", d); return 0; }
    char buf[24] = {};
    memcpy(buf, d->data + start, (size_t)numlen);
    errno = 0;
    int64_t result = strtoll(buf, nullptr, 10);
    // §2.15 (v0.314, fix #5400): strtoll saturates to LONG_MAX/MIN on overflow
    // and sets ERANGE.  Detect this so 9223372036854775808 into long raises
    // SerializationException instead of returning a wrong value.
    if (errno == ERANGE) {
        bz_ser_throw_at_json("JSON integer overflow (§2.15)", d);
        return 0;
    }
    return result;
}

static uint64_t bz_jder_parse_uint(bz_binder_t* d) {
    bz_jder_skip_ws(d);
    if (d->pos >= d->len || d->data[d->pos] < '0' || d->data[d->pos] > '9') {
        bz_ser_throw_at_json("Expected JSON unsigned integer (§2.15)", d);
        return 0;
    }
    int64_t start = d->pos;
    while (d->pos < d->len && d->data[d->pos] >= '0' && d->data[d->pos] <= '9') ++d->pos;
    // §2.15 (v0.314, fix #5400): no decimal point or exponent allowed.
    if (d->pos < d->len && (d->data[d->pos] == '.' || d->data[d->pos] == 'e' || d->data[d->pos] == 'E')) {
        bz_ser_throw_at_json("JSON unsigned integer cannot have fraction or exponent (§2.15)", d);
        return 0;
    }
    int64_t numlen = d->pos - start;
    if (numlen > 20) { bz_ser_throw_at_json("JSON uint overflow (§2.15)", d); return 0; }
    char buf[24] = {};
    memcpy(buf, d->data + start, (size_t)numlen);
    errno = 0;
    uint64_t result = strtoull(buf, nullptr, 10);
    // §2.15 (v0.314, fix #5400): strtoull saturates to ULONG_MAX on overflow and
    // sets ERANGE.  Detect this so 18446744073709551616 into ulong raises
    // SerializationException instead of returning a wrong value.
    if (errno == ERANGE) {
        bz_ser_throw_at_json("JSON uint overflow (§2.15)", d);
        return 0;
    }
    return result;
}

static double bz_jder_parse_float(bz_binder_t* d) {
    bz_jder_skip_ws(d);
    if (d->pos >= d->len) {
        bz_ser_throw_at_json("Expected JSON number (float), got end of input (§2.15)", d);
        return 0.0;
    }
    int64_t start = d->pos;
    if (d->data[d->pos] == '-') ++d->pos;
    while (d->pos < d->len && d->data[d->pos] >= '0' && d->data[d->pos] <= '9') ++d->pos;
    if (d->pos < d->len && d->data[d->pos] == '.') {
        ++d->pos;
        while (d->pos < d->len && d->data[d->pos] >= '0' && d->data[d->pos] <= '9') ++d->pos;
    }
    if (d->pos < d->len && (d->data[d->pos] == 'e' || d->data[d->pos] == 'E')) {
        ++d->pos;
        if (d->pos < d->len && (d->data[d->pos] == '+' || d->data[d->pos] == '-')) ++d->pos;
        while (d->pos < d->len && d->data[d->pos] >= '0' && d->data[d->pos] <= '9') ++d->pos;
    }
    int64_t numlen = d->pos - start;
    if (numlen > 50) { bz_ser_throw_at_json("JSON float too long (§2.15)", d); return 0.0; }
    char buf[64] = {};
    memcpy(buf, d->data + start, (size_t)numlen);
    // stdlib §2.15 (v0.314, #5200/#5369): JSON requires at least one digit
    // after '.', and at least one digit after the exponent sign.  Reject
    // forms like "1." and "1e" before calling strtod.
    char lastChar = buf[numlen - 1];
    if (lastChar == '.' || lastChar == 'e' || lastChar == 'E' ||
        lastChar == '+' || lastChar == '-') {
        bz_ser_throw_at_json("JSON float has invalid form (§2.15)", d);
        return 0.0;
    }
    double v = strtod(buf, nullptr);
    // stdlib §2.15 (v0.314, #5200/#5369): a JSON number whose rounded value
    // is not finite raises SerializationException.
    if (!std::isfinite(v)) {
        bz_ser_throw_at_json("JSON float is not finite (§2.15)", d);
        return 0.0;
    }
    return v;
}

// Parse a JSON number as a decimal, returns a pointer to a thread-local bz_decimal_t.
// §2.15: "WriteDecimal produces a JSON number written without precision loss."
static void* bz_jder_parse_decimal(bz_binder_t* d) {
    bz_jder_skip_ws(d);
    if (d->pos >= d->len) {
        bz_ser_throw_at_json("Expected JSON decimal number, got end of input (§2.15)", d);
        return nullptr;
    }
    int64_t start = d->pos;
    if (d->data[d->pos] == '-') ++d->pos;
    while (d->pos < d->len && d->data[d->pos] >= '0' && d->data[d->pos] <= '9') ++d->pos;
    if (d->pos < d->len && d->data[d->pos] == '.') {
        ++d->pos;
        while (d->pos < d->len && d->data[d->pos] >= '0' && d->data[d->pos] <= '9') ++d->pos;
    }
    int64_t numlen = d->pos - start;
    if (numlen > 50) { bz_ser_throw_at_json("JSON decimal too long (§2.15)", d); return nullptr; }
    char buf[64] = {};
    memcpy(buf, d->data + start, (size_t)numlen);
    // Use a thread-local buffer for the result (same pattern as bz_binder_read_decimal).
    static thread_local uint32_t jdec_buf[4];
    memset(jdec_buf, 0, sizeof(jdec_buf));
    bz_decimal_parse_cstr(buf, reinterpret_cast<bz_decimal_t*>(jdec_buf), nullptr, nullptr);
    return jdec_buf;
}

extern "C" {

void* bz_binder_alloc(const uint8_t* data, int64_t len, int8_t ignore_unknown) {
    auto* d = static_cast<bz_binder_t*>(calloc(1, sizeof(bz_binder_t)));
    d->data             = data;
    d->len              = len;
    d->pos              = 0;
    d->ignore_unknown   = ignore_unknown;
    d->kind             = 0;  // binary
    return d;
}

// JSON deserializer: data is a UTF-8 JSON byte span (borrowed from caller).
void* bz_jder_alloc(const uint8_t* data, int64_t len, int8_t ignore_unknown) {
    auto* d = static_cast<bz_binder_t*>(calloc(1, sizeof(bz_binder_t)));
    d->data                  = data;
    d->len                   = len;
    d->pos                   = 0;
    d->ignore_unknown        = ignore_unknown;
    d->allow_trailing_commas = 0;
    d->max_depth             = 64;
    d->kind                  = 1;  // JSON
    d->json_line             = 1;  // fix #688: 1-based position tracking
    d->json_col              = 1;
    return d;
}

// fix #687 / stdlib §2.15 JsonOptions: JSON deserializer with full options.
void* bz_jder_alloc_opts(const uint8_t* data, int64_t len,
                          int8_t ignore_unknown, int8_t allow_trailing_commas,
                          int32_t max_depth) {
    auto* d = static_cast<bz_binder_t*>(calloc(1, sizeof(bz_binder_t)));
    d->data                  = data;
    d->len                   = len;
    d->pos                   = 0;
    d->ignore_unknown        = ignore_unknown;
    d->allow_trailing_commas = allow_trailing_commas;
    d->max_depth             = (max_depth > 0) ? max_depth : 64;
    d->kind                  = 1;  // JSON
    d->json_line             = 1;  // fix #688: 1-based position tracking
    d->json_col              = 1;
    return d;
}

// JSON deserializer from an owned malloc'd NUL-terminated UTF-8 string.
// Takes ownership of `utf8z`; frees it in bz_binder_free.
// Used by new JsonDeserializer(string) codegen path.
void* bz_jder_alloc_str(char* utf8z, int64_t len, int8_t ignore_unknown) {
    auto* d = static_cast<bz_binder_t*>(calloc(1, sizeof(bz_binder_t)));
    d->data                  = reinterpret_cast<const uint8_t*>(utf8z);
    d->len                   = len;
    d->pos                   = 0;
    d->ignore_unknown        = ignore_unknown;
    d->allow_trailing_commas = 0;
    d->max_depth             = 64;
    d->kind                  = 2;  // JSON + owns data (free on bz_binder_free)
    d->json_line             = 1;  // fix #688: 1-based position tracking
    d->json_col              = 1;
    return d;
}

// fix #687 / stdlib §2.15 JsonOptions: JSON deserializer from owned string with options.
void* bz_jder_alloc_str_opts(char* utf8z, int64_t len,
                              int8_t ignore_unknown, int8_t allow_trailing_commas,
                              int32_t max_depth) {
    auto* d = static_cast<bz_binder_t*>(calloc(1, sizeof(bz_binder_t)));
    d->data                  = reinterpret_cast<const uint8_t*>(utf8z);
    d->len                   = len;
    d->pos                   = 0;
    d->ignore_unknown        = ignore_unknown;
    d->allow_trailing_commas = allow_trailing_commas;
    d->max_depth             = (max_depth > 0) ? max_depth : 64;
    d->kind                  = 2;  // JSON + owns data (free on bz_binder_free)
    d->json_line             = 1;  // fix #688: 1-based position tracking
    d->json_col              = 1;
    return d;
}

void bz_binder_free(void* derp) {
    if (!derp) return;
    auto* d = static_cast<bz_binder_t*>(derp);
    free(d->tag_ptrs);
    free(d->tag_in_progress);
    if (d->kind == 2) {
        // Owns the UTF-8 string buffer (allocated by bz_string_to_utf8z).
        free(const_cast<uint8_t*>(d->data));
    }
    free(d);
}

int64_t bz_binder_read_int(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) return bz_jder_parse_int(d);
    // fix #688 / stdlib §2.15: binary truncation → byte offset in message.
    int64_t off = d->pos;
    uint64_t v = 0;
    if (!bz_binder_read_le64_impl(d, &v)) {
        bz_ser_throw_at_binary("Truncated binary input reading int (§2.15)", off);
    }
    return (int64_t)v;
}

uint64_t bz_binder_read_uint(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) return bz_jder_parse_uint(d);
    int64_t off = d->pos;
    uint64_t v = 0;
    if (!bz_binder_read_le64_impl(d, &v)) {
        bz_ser_throw_at_binary("Truncated binary input reading uint (§2.15)", off);
    }
    return v;
}

double bz_binder_read_float(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) return bz_jder_parse_float(d);
    int64_t off = d->pos;
    uint64_t bits = 0;
    if (!bz_binder_read_le64_impl(d, &bits)) {
        bz_ser_throw_at_binary("Truncated binary input reading float (§2.15)", off);
    }
    double v;
    memcpy(&v, &bits, 8);
    return v;
}

// fix #665 / §2.14 / §9.8: ReadDecimal — reads 4×u32 LE (16 bytes) from the
// wire.  Returns a pointer to a thread-local static 16-byte buffer holding the
// decoded {lo32, mid32, hi32, flags} struct (the bz.decimal layout).  The
// caller (codegen) loads the struct from this pointer immediately (no ownership
// transfer; the buffer is overwritten on the next call).
void* bz_binder_read_decimal(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) return bz_jder_parse_decimal(d);
    static thread_local uint32_t dec_buf[4];
    // fix #2394 / §2.15 (v0.235): each of the 4 LE32 words was read unchecked,
    // so a truncated wire silently left the tail word(s) zeroed instead of
    // raising — e.g. cutting the flags word zeroes the decimal's scale,
    // turning 3.14m into the wrong value 314m with NO exception (silent data
    // corruption, not just a missing-value default). Mirror bz_binder_read_int
    // /_uint/_float: report the offset of the decimal's first byte, matching
    // the "off = start of this value" convention used there and in
    // bz_binder_read_bz_string above.
    int64_t off = d->pos;
    for (int i = 0; i < 4; ++i) {
        uint32_t v = 0;
        if (!bz_binder_read_le32_impl(d, &v)) {
            bz_ser_throw_at_binary("Truncated binary input reading decimal (§2.15)", off);
            return dec_buf;  // pending exception; caller's emitPendingCheck() discards this
        }
        dec_buf[i] = v;
    }
    return dec_buf;
}

int8_t bz_binder_read_bool(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        bz_jder_skip_ws(d);
        if (d->pos + 4 <= d->len && memcmp(d->data + d->pos, "true", 4) == 0) {
            d->pos += 4; return 1;
        }
        if (d->pos + 5 <= d->len && memcmp(d->data + d->pos, "false", 5) == 0) {
            d->pos += 5; return 0;
        }
        bz_ser_throw_at_json("Expected JSON bool (§2.15)", d);
        return 0;
    }
    // fix #2394 / §2.15 (v0.235): the byte read was unchecked, so truncation
    // silently returned the default `false` instead of raising — mirror
    // bz_binder_read_int/_uint/_float (and the JSON branch above, which
    // already throws on its own malformed-input shape).
    int64_t off = d->pos;
    uint8_t b = 0;
    if (!bz_binder_read_byte_impl(d, &b)) {
        bz_ser_throw_at_binary("Truncated binary input reading bool (§2.15)", off);
    }
    return b ? 1 : 0;
}

// Returns a newly-allocated bz_string_struct* (caller owns).
void* bz_binder_read_string(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        bz_jder_skip_ws(d);
        return bz_jder_parse_string(d);
    }
    return bz_binder_read_bz_string(d);
}

int32_t bz_binder_read_char(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        // Parse a one-character JSON string.
        bz_jder_skip_ws(d);
        int64_t ncp = 0;
        uint32_t* cps = bz_jder_parse_string_cps(d, &ncp);
        if (!cps) return 0;
        uint32_t cp = (ncp > 0) ? cps[0] : 0;
        free(cps);
        return (int32_t)cp;
    }
    auto* str = static_cast<bz_string_struct*>(
        bz_binder_read_bz_string(d));
    if (!str || str->len == 0) {
        if (str) { free(str->data); free(str); }
        return 0;
    }
    const uint8_t* u = static_cast<const uint8_t*>(str->data);
    uint32_t cp;
    if      (u[0] < 0x80) { cp = u[0]; }
    else if (u[0] < 0xE0) { cp = ((u[0]&0x1F)<<6)  | (u[1]&0x3F); }
    else if (u[0] < 0xF0) { cp = ((u[0]&0x0F)<<12) | ((u[1]&0x3F)<<6) | (u[2]&0x3F); }
    else                  { cp = ((u[0]&0x07)<<18)  | ((u[1]&0x3F)<<12) | ((u[2]&0x3F)<<6) | (u[3]&0x3F); }
    free(str->data);
    free(str);
    return (int32_t)cp;
}

int8_t bz_binder_try_read_null(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        bz_jder_skip_ws(d);
        if (d->pos + 4 <= d->len && memcmp(d->data + d->pos, "null", 4) == 0) {
            d->pos += 4; return 1;
        }
        return 0;
    }
    if (d->pos >= d->len) return 0;
    if (d->data[d->pos] == 0) { d->pos++; return 1; }
    return 0;
}

// Count the number of top-level key-value pairs in a JSON object starting
// right after '{' at current pos.  Does NOT advance d->pos.
// Used to pre-count fields so the synthesized for-loop runs the right number
// of times.  The actual parsing is done field-by-field in ReadFieldName.
static int32_t bz_jder_count_object_fields(bz_binder_t* d) {
    // Save pos, scan forward, restore.
    int64_t saved = d->pos;

    int32_t count = 0;
    int     depth = 1;  // we are inside the opening '{'

    bz_jder_skip_ws(d);
    if (d->pos < d->len && d->data[d->pos] == '}') {
        d->pos = saved;
        return 0;
    }

    // Count comma-separated key-value pairs at depth 1.
    // We need a proper scan because values can be nested objects/arrays.
    while (d->pos < d->len && depth > 0) {
        uint8_t c = d->data[d->pos];
        if (c == '"') {
            // String: skip over it.
            ++d->pos;
            while (d->pos < d->len) {
                uint8_t s = d->data[d->pos++];
                if (s == '\\') { if (d->pos < d->len) ++d->pos; }
                else if (s == '"') break;
            }
        } else if (c == '{' || c == '[') {
            ++depth; ++d->pos;
        } else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) { ++d->pos; break; }
            ++d->pos;
        } else if (c == ':' && depth == 1) {
            // The ':' that separates key from value at the top level.
            ++d->pos;
            count++;  // each ':' at depth 1 = one field.
        } else {
            ++d->pos;
        }
    }

    // fix #688 / §2.15: if we exhausted input without closing '}', the JSON is
    // truncated.  Throw at current position (before restoring), so
    // bz_ser_throw_at_json reports the correct end-of-input location.
    if (depth > 0) {
        bz_ser_throw_at_json("Truncated JSON: unexpected end of input inside object (§2.15)", d);
        d->pos = saved;
        return -1;
    }

    d->pos = saved;
    return count;
}

int32_t bz_binder_begin_object(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);

    // §9.8 tier-4 polymorphic / choice ($case): poly_preread_count is set by
    // bz_binder_read_poly_discriminator / bz_binder_read_case_discriminator after
    // consuming '{' and the "$type"/"$case" field.  Use it directly without
    // reading the stream again.  The active flag (not `count > 0`) gates the
    // preread so a ZERO remaining-field count — a payload-free choice case — is
    // honoured (§9.8, #2046).
    if (d->poly_preread_active) {
        int32_t n = d->poly_preread_count;
        d->poly_preread_count = 0;
        d->poly_preread_active = 0;
        d->in_object = true;
        // For JSON: json_obj_preread is already set to 1 by
        // bz_binder_read_poly_discriminator ('{' already consumed).
        if (d->kind >= 1) {
            d->json_obj_preread = 0;  // already cleared (we return n directly)
        }
        return n;
    }

    if (d->kind >= 1) {
        // §2.15: for JSON we pre-count fields so the synthesized for-loop runs
        // the right number of times.  The loop is: idx in [0, fieldCount), and
        // each iteration calls ReadFieldName + dispatch + SkipValue/ReadXxx.
        //
        // §2.15 v0.139 graph-identity: bz_binder_read_graph_marker may have
        // already consumed the `{` and the `"$id":n,` pseudo-field, leaving the
        // position just after the comma.  In that case json_obj_preread=1 and we
        // must NOT read `{` again; we just count the remaining real fields.
        if (d->json_obj_preread) {
            d->json_obj_preread = 0;
            d->in_object = true;
            int32_t cnt = bz_jder_count_object_fields(d);
            return cnt;
        }
        bz_jder_skip_ws(d);
        if (d->pos >= d->len || d->data[d->pos] != '{') {
            bz_ser_throw_at_json("Expected '{' in JSON object (§2.15)", d);
            return -1;
        }
        ++d->pos;  // skip '{'
        d->in_object = true;
        int32_t cnt = bz_jder_count_object_fields(d);
        return cnt;
    }
    // fix #688 / §2.15: binary truncation → report byte offset.
    int64_t off = d->pos;
    uint32_t cnt = 0;
    if (!bz_binder_read_le32_impl(d, &cnt)) {
        bz_ser_throw_at_binary("Truncated binary input at object header (§2.15)", off);
        return -1;
    }
    d->in_object = true;
    return (int32_t)cnt;
}

// Returns a newly-allocated bz_string* for the field name.
// JSON: skip ws, check if next char is '}' (object end → return null to signal
// the loop to stop).  Otherwise parse "key": and return the key string.
// The synthesized deserialization loop uses the returned count from BeginObject
// (-1 for JSON) and the loop iterates until... wait, the synthesized loop uses
// the returned field count from begin_object to drive the for-loop.
// For JSON, the returned count is -1. This means the loop would run -1 times
// which is 0 (since LLVM compiles `idx < -1` as always false with signed ints).
//
// This is a problem: the synthesized loop is `for idx in [0, fieldCount)`.
// For JSON with fieldCount=-1, the loop body never runs.
//
// SOLUTION: For JSON mode, we return the actual number of fields in the object.
// But we don't know it in advance. Instead, we must change the loop termination.
//
// §2.15: "consumers use ReadFieldName() driven by the object's textual extent".
// This means the synthesized loop must be driven by HasMoreFields() for JSON.
// But the current synthesis emitters emit a counted for-loop.
//
// ARCHITECTURAL NOTE: The current synthesis uses a counted for-loop
//   idx = 0; while (idx < fieldCount) { readFieldName(); dispatch; idx++; }
// For JSON, fieldCount=-1 makes the loop run 0 times (signed comparison: 0 < -1 is false).
//
// To fix this without changing the synthesis emitters, we count the fields
// in BeginObject for JSON: parse {, then pre-scan to count key-value pairs.
// But that requires two passes over the JSON.
//
// Alternative: return a large count (e.g., INT32_MAX) and detect end-of-object
// in ReadFieldName (return null = loop stops via the null-check in emitClassDeserialize).
// The record deserializer also checks: the loop header terminates on `idx < fieldCount`.
//
// Better: BeginObject for JSON pre-scans the object to count fields.
// This is O(n) but correct and simple.
//
// Actually, looking at emitRecordDeserialize more carefully: after the loop
// runs, EndObject is called. If fieldCount=-1 and the loop doesn't run,
// then EndObject is called immediately. For JSON, EndObject would consume '}'.
// But we haven't consumed any fields. This leaves the object unparsed.
//
// FINAL DECISION: bz_binder_begin_object for JSON does a lightweight pre-scan
// to count top-level key-value pairs. We save the start position, count,
// then restore the position so the normal read path proceeds from '{'.
// Actually simpler: scan forward counting ',' at depth 1, return count+1
// if non-empty, 0 if empty.
//
// This is the cleanest approach that keeps synthesis unchanged.
void* bz_binder_read_field_name(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        bz_jder_skip_ws(d);
        // Skip comma between fields (present for field N>0).
        // The comma is NOT present before the first field.
        if (d->pos < d->len && d->data[d->pos] == ',') {
            ++d->pos;
            bz_jder_skip_ws(d);
        }
        // Check for end-of-object (should not happen if we pre-counted correctly).
        if (d->pos >= d->len || d->data[d->pos] == '}') {
            return nullptr;  // null → truncated-input path in synthesized deserializer
        }
        // Parse the key string.
        void* key = bz_jder_parse_string(d);
        if (!key) return nullptr;
        // Skip ':'.
        if (!bz_jder_expect_char(d, ':')) {
            bz_string_release(key);
            return nullptr;
        }
        return key;
    }
    return bz_binder_read_bz_string(d);
}

// skip_value: advance past one value at current position.
void bz_binder_skip_value(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        // §2.15: SkipValue — unknown field; if ignore_unknown=0 → raise.
        if (!d->ignore_unknown) {
            bz_ser_throw_at_json("Unknown field in JSON input (§2.15)", d);
            return;
        }
        bz_jder_skip_ws(d);
        bz_jder_skip_value(d);
        return;
    }
    if (d->pos >= d->len) return;
    uint32_t maybe_len = 0;
    if (!bz_binder_read_le32_impl(d, &maybe_len)) return;
    if (d->pos + (int64_t)maybe_len <= d->len)
        d->pos += (int64_t)maybe_len;
}

void bz_binder_end_object(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        // Skip ws and consume '}'.
        bz_jder_skip_ws(d);
        // §2.15 JsonOptions.AllowTrailingCommas: a trailing comma immediately
        // before '}' is legal when allow_trailing_commas is set, and an error
        // when it is not.
        if (d->pos < d->len && d->data[d->pos] == ',') {
            if (!d->allow_trailing_commas) {
                bz_ser_throw_at_json("Trailing comma in JSON object (§2.15)", d);
                d->in_object = false;
                return;
            }
            ++d->pos;  // skip trailing comma
            bz_jder_skip_ws(d);
        }
        if (d->pos < d->len && d->data[d->pos] == '}') ++d->pos;
        d->in_object = false;
        return;
    }
    d->in_object = false;
    d->fields_remaining = 0;
}

int32_t bz_binder_begin_array(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        if (!bz_jder_expect_char(d, '[')) return -1;
        return -1;  // count unknown
    }
    uint32_t cnt = 0;
    if (!bz_binder_read_le32_impl(d, &cnt)) return -1;
    return (int32_t)cnt;
}

int8_t bz_binder_has_more_elements(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        // §2.15: HasMoreElements — peek for ']'.
        bz_jder_skip_ws(d);
        if (d->pos >= d->len || d->data[d->pos] == ']') return 0;
        return 1;
    }
    // Count-prefixed format: caller drives the loop; this is a placeholder.
    return 0;
}

void bz_binder_end_array(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);
    if (d->kind >= 1) {
        bz_jder_skip_ws(d);
        if (d->pos < d->len && d->data[d->pos] == ']') ++d->pos;
        return;
    }
    /* no trailer */
}

// ---------------------------------------------------------------------------
// §9.8 tier-3: identity-tag graph deserialization helpers
//
// Wire format (per §9.8 / tier-3 design):
//   Before each class-typed component in `serialized graph` mode, the wire
//   carries one byte marker:
//     BZ_SER_INLINE (0x01) + LE32 tag_id  → inline object follows; register tag
//     BZ_SER_REF    (0x02) + LE32 tag_id  → back-edge; resolve from table
//
// The tag table (tag_ptrs, tag_in_progress) is embedded in bz_binder_t and
// grows dynamically; tag_id is 1-based (slot index = tag_id - 1).
// ---------------------------------------------------------------------------

// Ensure the tag table has room for at least `tag_id` entries.
static void bz_binder_graph_ensure_slot(bz_binder_t* d, int32_t tag_id) {
    if (tag_id <= d->tag_count) return;
    // Grow both parallel arrays to cover slots 0 .. tag_id-1.
    void** new_ptrs = static_cast<void**>(
        realloc(d->tag_ptrs, (size_t)tag_id * sizeof(void*)));
    int8_t* new_ip = static_cast<int8_t*>(
        realloc(d->tag_in_progress, (size_t)tag_id * sizeof(int8_t)));
    // Zero-initialize the newly added slots.
    for (int32_t i = d->tag_count; i < tag_id; ++i) {
        new_ptrs[i] = nullptr;
        new_ip[i]   = 0;
    }
    d->tag_ptrs        = new_ptrs;
    d->tag_in_progress = new_ip;
    d->tag_count       = tag_id;
}

// Read the graph marker byte + tag_id from the wire.
// Returns:
//   BZ_SER_INLINE (1) → inline object; *out_tag set to the tag_id
//   BZ_SER_REF    (2) → back-edge ref; *out_tag set to the tag_id
//   0             → truncated / unexpected input
//
// §2.15 v0.139 JSON form:
//   INLINE: the next JSON value is an object whose first field is `"$id":n`.
//     We consume `{` + `"$id":n` and optionally the trailing `,`.  Set
//     json_obj_preread=1 so bz_binder_begin_object skips re-reading `{`.
//     Returns BZ_SER_INLINE; *out_tag = n.
//   REF: the next JSON value is `{"$ref":n}`.
//     We consume the entire object.  Returns BZ_SER_REF; *out_tag = n.
//   Malformed / missing: returns 0, *out_tag = 0.
int32_t bz_binder_read_graph_marker(void* derp, int32_t* out_tag) {
    auto* d = static_cast<bz_binder_t*>(derp);
    *out_tag = 0;

    if (d->kind >= 1) {
        // JSON path — per §2.15 v0.139.
        bz_jder_skip_ws(d);
        if (d->pos >= d->len || d->data[d->pos] != '{') {
            bz_ser_throw_at_json("Expected '{' for JSON graph-identity token (§2.15 v0.139)",
                                 d);
            return 0;
        }
        ++d->pos;  // consume '{'

        // Parse the first field name (must be "$id" or "$ref").
        bz_jder_skip_ws(d);
        if (d->pos >= d->len || d->data[d->pos] != '"') {
            bz_ser_throw_at_json("Expected '\"' in JSON graph-identity object (§2.15 v0.139)",
                                 d);
            return 0;
        }
        // Read the key without allocating: find the closing '"'.
        ++d->pos;  // skip opening '"'
        // Collect key bytes up to the closing '"' (no escape handling needed for
        // the reserved "$id"/"$ref" keys which contain only ASCII).
        const int64_t keyStart = d->pos;
        while (d->pos < d->len && d->data[d->pos] != '"') ++d->pos;
        const int64_t keyLen = d->pos - keyStart;
        if (d->pos < d->len) ++d->pos;  // skip closing '"'

        // Determine which token this is.
        bool isId  = (keyLen == 3 &&
                      d->data[keyStart+0] == '$' &&
                      d->data[keyStart+1] == 'i' &&
                      d->data[keyStart+2] == 'd');
        bool isRef = (keyLen == 4 &&
                      d->data[keyStart+0] == '$' &&
                      d->data[keyStart+1] == 'r' &&
                      d->data[keyStart+2] == 'e' &&
                      d->data[keyStart+3] == 'f');

        if (!isId && !isRef) {
            bz_ser_throw_at_json("Expected \"$id\" or \"$ref\" as first field in JSON "
                                 "graph-identity object (§2.15 v0.139)", d);
            return 0;
        }

        // Skip ':' between key and value.
        if (!bz_jder_expect_char(d, ':')) return 0;

        // Parse the integer tag_id.
        int64_t tag_id = bz_jder_parse_int(d);
        *out_tag = (int32_t)tag_id;

        if (isRef) {
            // Ref: consume the closing '}' (the entire {"$ref":n} object).
            bz_jder_skip_ws(d);
            if (d->pos < d->len && d->data[d->pos] == '}') ++d->pos;
            return BZ_SER_REF;
        }

        // Inline ("$id"):  the object continues with real fields.
        // Consume optional ',' after the "$id":n entry.
        bz_jder_skip_ws(d);
        if (d->pos < d->len && d->data[d->pos] == ',') ++d->pos;
        // Mark that begin_object must skip reading '{' (already consumed).
        d->json_obj_preread = 1;
        return BZ_SER_INLINE;
    }

    // Binary path (original implementation).
    uint8_t marker = 0;
    if (!bz_binder_read_byte_impl(d, &marker)) { *out_tag = 0; return 0; }
    uint32_t tag_id = 0;
    if (!bz_binder_read_le32_impl(d, &tag_id)) { *out_tag = 0; return 0; }
    *out_tag = (int32_t)tag_id;
    return (int32_t)marker;
}

// Mark tag_id as "currently being constructed" (cycle-detection sentinel).
// Called just before beginning to construct the object for this tag.
void bz_binder_graph_mark_in_progress(void* derp, int32_t tag_id) {
    if (!derp || tag_id <= 0) return;
    auto* d = static_cast<bz_binder_t*>(derp);
    bz_binder_graph_ensure_slot(d, tag_id);
    d->tag_in_progress[tag_id - 1] = 1;
    d->tag_ptrs[tag_id - 1]        = nullptr;
}

// Register the constructed object pointer for tag_id (construction complete).
// Clears the in-progress flag.
void bz_binder_graph_register_tag(void* derp, int32_t tag_id, void* ptr) {
    if (!derp || tag_id <= 0) return;
    auto* d = static_cast<bz_binder_t*>(derp);
    bz_binder_graph_ensure_slot(d, tag_id);
    d->tag_ptrs[tag_id - 1]        = ptr;
    d->tag_in_progress[tag_id - 1] = 0;
}

// Resolve a tag_id to its object pointer.
// Returns NULL if:
//   - tag_id out of range (not yet registered) → indicates a cycle or bad input
//   - tag_in_progress[tag_id-1] = 1 → cycle: object still being constructed
// Per §9.8: a cycle raises SerializationException; the codegen checks for NULL
// and throws.
void* bz_binder_graph_resolve_tag(void* derp, int32_t tag_id) {
    if (!derp || tag_id <= 0) return nullptr;
    auto* d = static_cast<bz_binder_t*>(derp);
    if (tag_id > d->tag_count)     return nullptr;  // not yet registered
    if (d->tag_in_progress[tag_id - 1]) return nullptr;  // cycle
    return d->tag_ptrs[tag_id - 1];
}

// §9.8 tier-4: polymorphic-dispatch deserialization entry point.
//
// Reads the opening object and the "$type" discriminator field, then sets up
// state so the concrete type's synthesized Deserialize (which calls
// bz_binder_begin_object) reads the REMAINING fields without re-consuming
// the object header.
//
// Binary: reads LE32(field_count), reads field-name ("$type"), reads discriminator
//         string.  Sets poly_preread_count = field_count - 1 so the concrete's
//         bz_binder_begin_object returns field_count - 1.
//
// JSON:   reads '{', pre-counts fields (N), reads "$type":value, sets
//         poly_preread_count = N - 1 so the concrete's bz_binder_begin_object
//         returns N - 1 without re-reading '{' (json_obj_preread is NOT set since
//         poly_preread_count is the primary gate).
//
// Returns: a newly-allocated bz_string* (rc=1) holding the discriminator value.
//          The caller must bz_string_release it.  Returns nullptr and throws
//          SerializationException if the first field is not "$type" or input
//          is truncated.
//
// After this call, invoke the concrete type's synthesized Deserialize(d) as
// usual; its bz_binder_begin_object will consume poly_preread_count and return
// the pre-set value (without reading another object header from the stream).
// The concrete's bz_binder_end_object closes the same outer object.
void* bz_binder_read_poly_discriminator(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);

    if (d->kind >= 1) {
        // JSON path.
        // §9.8 / §2.15 v0.139: when the graph-identity marker reader already consumed
        // '{' and "$id":n (setting json_obj_preread=1), we must NOT read '{' again.
        // Clear the preread flag and continue reading the next field ("$type").
        bool preread = (d->json_obj_preread != 0);
        if (preread) {
            d->json_obj_preread = 0;  // consumed
            d->in_object = true;
        } else {
            bz_jder_skip_ws(d);
            if (d->pos >= d->len || d->data[d->pos] != '{') {
                bz_ser_throw("Polymorphic deserialize: expected '{' (§9.8)", nullptr);
                return nullptr;
            }
            ++d->pos;  // consume '{'
            d->in_object = true;
        }

        // Pre-count the remaining fields (from current position).
        int32_t total_count = bz_jder_count_object_fields(d);
        // fix #2432 (§9.8 / stdlib §2.15, v0.235): mirrors #2427's fix on the
        // "$case" sibling below (bz_binder_read_case_discriminator) — LSan-
        // confirmed via a truncated {"$type":"leaf","V":9 (missing '}'): the
        // pre-count itself raises internally (and returns -1) on truncation, an
        // already-in-flight exception the unconditional bz_ser_throw a few lines
        // down would otherwise supersede/orphan. Bail out before re-inspecting.
        if (bz_exc) return nullptr;
        if (total_count <= 0) {
            bz_ser_throw("Polymorphic deserialize: empty object, missing \"$type\" field (§9.8)",
                         nullptr);
            return nullptr;
        }

        // Read the first field name — must be "$type".
        bz_jder_skip_ws(d);
        // Skip optional comma before the first field in preread mode (there should
        // not be one at the first field, but handle defensively).
        if (d->pos < d->len && d->data[d->pos] == ',') ++d->pos;
        bz_jder_skip_ws(d);
        // Check for '}' (empty object).
        if (d->pos >= d->len || d->data[d->pos] == '}') {
            bz_ser_throw("Polymorphic deserialize: missing \"$type\" field (§9.8)", nullptr);
            return nullptr;
        }
        // Parse the key (without allocating: just verify it's "$type").
        if (d->data[d->pos] != '"') {
            bz_ser_throw("Polymorphic deserialize: expected '\"' for field name (§9.8)", nullptr);
            return nullptr;
        }
        ++d->pos;  // skip opening '"'
        const int64_t kStart = d->pos;
        while (d->pos < d->len && d->data[d->pos] != '"') ++d->pos;
        const int64_t kLen = d->pos - kStart;
        if (d->pos < d->len) ++d->pos;  // skip closing '"'
        // Verify key is "$type".
        bool isTypeKey = (kLen == 5 &&
                          d->data[kStart+0] == '$' &&
                          d->data[kStart+1] == 't' &&
                          d->data[kStart+2] == 'y' &&
                          d->data[kStart+3] == 'p' &&
                          d->data[kStart+4] == 'e');
        if (!isTypeKey) {
            bz_ser_throw("Polymorphic deserialize: first field must be \"$type\" (§9.8)", nullptr);
            return nullptr;
        }
        // Skip ':'.
        if (!bz_jder_expect_char(d, ':')) {
            // fix #2432 (§9.8 / stdlib §2.15, v0.235): bz_jder_expect_char already
            // raised internally (bz_ser_throw_at_json) on failure; bail out before
            // the manual bz_ser_throw below supersedes it (Bug-A pattern). By
            // inspection, byte-identical in shape to the LSan-confirmed "$case"
            // sibling a few lines down (bz_binder_read_case_discriminator).
            if (bz_exc) return nullptr;
            bz_ser_throw("Polymorphic deserialize: expected ':' after \"$type\" (§9.8)", nullptr);
            return nullptr;
        }
        // fix #2419 (§9.8 / stdlib §2.15, v0.235): skip whitespace between the
        // "$type" colon and its value — the identical gap flagged (but not
        // independently confirmed) on this function's "$case" sibling
        // (bz_binder_read_case_discriminator, above); see the fix comment
        // there for the full rationale.
        bz_jder_skip_ws(d);
        // Read the discriminator string value.
        void* disc = bz_jder_parse_string(d);
        if (!disc) {
            // fix #2432 (§9.8 / stdlib §2.15, v0.235): bz_jder_parse_string raises
            // internally on every malformed-input path; the one exception (OOM in
            // bz_jder_cps_to_bz_string) returns nullptr WITHOUT throwing, so the
            // manual bz_ser_throw below remains the correct safety net for THAT
            // case. Bail out only when an exception is already in flight. By
            // inspection, byte-identical in shape to the LSan-confirmed "$case"
            // sibling a few lines down (bz_binder_read_case_discriminator).
            if (bz_exc) return nullptr;
            bz_ser_throw("Polymorphic deserialize: failed to read \"$type\" value (§9.8)", nullptr);
            return nullptr;
        }
        // Skip optional ',' after "$type" value.
        bz_jder_skip_ws(d);
        if (d->pos < d->len && d->data[d->pos] == ',') ++d->pos;

        // Set preread_count so concrete's begin_object returns total_count-1.
        d->poly_preread_count = total_count - 1;
        d->poly_preread_active = 1;  // honour a zero remaining-field count (#2046)
        // Note: do NOT set json_obj_preread here — poly_preread_count gates the
        // begin_object path directly without going through the JSON preread path.
        return disc;
    }

    // Binary path.
    uint32_t field_count = 0;
    if (!bz_binder_read_le32_impl(d, &field_count)) {
        bz_ser_throw("Polymorphic deserialize: truncated binary object header (§9.8)", nullptr);
        return nullptr;
    }
    if (field_count == 0) {
        bz_ser_throw("Polymorphic deserialize: empty object, missing \"$type\" field (§9.8)", nullptr);
        return nullptr;
    }
    // Read field name (should be "$type").
    void* keyStr = bz_binder_read_bz_string(d);
    if (!keyStr) {
        bz_ser_throw("Polymorphic deserialize: missing field name (§9.8)", nullptr);
        return nullptr;
    }
    // Verify key is "$type" (length 5, ASCII).
    const auto* kbz = static_cast<const bz_string_struct*>(keyStr);
    bool isTypeKey = (kbz->len == 5 && kbz->width == 1);
    if (isTypeKey) {
        const uint8_t* kd = static_cast<const uint8_t*>(kbz->data);
        isTypeKey = (kd[0]=='$' && kd[1]=='t' && kd[2]=='y' && kd[3]=='p' && kd[4]=='e');
    }
    bz_string_release(keyStr);
    if (!isTypeKey) {
        bz_ser_throw("Polymorphic deserialize: first binary field must be \"$type\" (§9.8)", nullptr);
        return nullptr;
    }
    // Read discriminator string value.
    void* disc = bz_binder_read_bz_string(d);
    if (!disc) {
        bz_ser_throw("Polymorphic deserialize: failed to read \"$type\" binary value (§9.8)", nullptr);
        return nullptr;
    }
    // Set preread_count so concrete's begin_object returns field_count-1.
    d->poly_preread_count = (int32_t)(field_count - 1);
    d->poly_preread_active = 1;  // honour a zero remaining-field count (#2046)
    d->in_object = true;
    return disc;
}

// §9.8 choice serialization (v0.200): read the leading "$case" discriminator of a
// choice's serialized object form and set up poly_preread_count so the following
// bz_binder_begin_object returns the remaining (payload) field count without
// re-reading the object header.  Structurally identical to
// bz_binder_read_poly_discriminator, but the reserved key it requires is "$case"
// (stdlib §2.15's $-reserved set) instead of "$type".
//
// Per §9.8: the discriminator is read FIRST, consumed by case dispatch before
// any payload binding; an object that does not lead with "$case" (or is empty)
// raises SerializationException.  The returned bz_string* (rc=1) holds the case
// discriminator value (the case's declared name or its `serialized "<name>"`
// rename); the caller dispatches on it and must bz_string_release it.
void* bz_binder_read_case_discriminator(void* derp) {
    auto* d = static_cast<bz_binder_t*>(derp);

    if (d->kind >= 1) {
        // JSON path.  (A value-like choice is a value type — it never carries a
        // "$id" identity tag — so the graph-marker preread does not apply here;
        // we still honour json_obj_preread defensively, mirroring the poly path.)
        bool preread = (d->json_obj_preread != 0);
        if (preread) {
            d->json_obj_preread = 0;  // consumed
            d->in_object = true;
        } else {
            bz_jder_skip_ws(d);
            if (d->pos >= d->len || d->data[d->pos] != '{') {
                bz_ser_throw("Choice deserialize: expected '{' (§9.8)", nullptr);
                return nullptr;
            }
            ++d->pos;  // consume '{'
            d->in_object = true;
        }

        int32_t total_count = bz_jder_count_object_fields(d);
        // fix #2427 (§9.8 / stdlib §2.15, v0.235): bz_jder_count_object_fields
        // itself raises SerializationException (and returns -1) when the object
        // is truncated before its closing '}' — an already-in-flight exception.
        // total_count == -1 also satisfies `total_count <= 0` below, so without
        // this check the unconditional bz_ser_throw a few lines down would
        // SUPERSEDE that exception with a second, unrelated one ("missing
        // \"$case\"") — orphaning/leaking the first exception object (its own
        // heap-allocated message string included). Bail out immediately instead,
        // mirroring the pending-check idiom used throughout this file (e.g.
        // bezel_runtime_decimal_guid.cpp's `if (bz_exc) return 0;`) and the
        // RECORD-side sibling (bz_binder_begin_object's JSON path), which does
        // not double-throw because it propagates total_count/-1 straight to its
        // codegen caller's own emitPendingCheck() rather than re-inspecting it.
        if (bz_exc) return nullptr;
        if (total_count <= 0) {
            bz_ser_throw("Choice deserialize: empty object, missing \"$case\" "
                         "discriminator (§9.8)", nullptr);
            return nullptr;
        }

        bz_jder_skip_ws(d);
        if (d->pos < d->len && d->data[d->pos] == ',') ++d->pos;
        bz_jder_skip_ws(d);
        if (d->pos >= d->len || d->data[d->pos] == '}') {
            bz_ser_throw("Choice deserialize: missing \"$case\" discriminator (§9.8)",
                         nullptr);
            return nullptr;
        }
        if (d->data[d->pos] != '"') {
            bz_ser_throw("Choice deserialize: expected '\"' for field name (§9.8)",
                         nullptr);
            return nullptr;
        }
        ++d->pos;  // skip opening '"'
        const int64_t kStart = d->pos;
        while (d->pos < d->len && d->data[d->pos] != '"') ++d->pos;
        const int64_t kLen = d->pos - kStart;
        if (d->pos < d->len) ++d->pos;  // skip closing '"'
        bool isCaseKey = (kLen == 5 &&
                          d->data[kStart+0] == '$' &&
                          d->data[kStart+1] == 'c' &&
                          d->data[kStart+2] == 'a' &&
                          d->data[kStart+3] == 's' &&
                          d->data[kStart+4] == 'e');
        if (!isCaseKey) {
            // §9.8: dispatch must select the case before component binding can
            // begin — an object that does not lead with the discriminator faults.
            bz_ser_throw("Choice deserialize: object does not lead with the "
                         "\"$case\" discriminator (§9.8)", nullptr);
            return nullptr;
        }
        if (!bz_jder_expect_char(d, ':')) {
            // fix #2432 (§9.8 / stdlib §2.15, v0.235): bz_jder_expect_char already
            // raised internally (bz_ser_throw_at_json) on failure; bail out before
            // the manual bz_ser_throw below supersedes it (Bug-A pattern,
            // LSan-confirmed: a non-colon byte after "$case" leaked 123 bytes /
            // 3 allocations behind the wrong exception message).
            if (bz_exc) return nullptr;
            bz_ser_throw("Choice deserialize: expected ':' after \"$case\" (§9.8)",
                         nullptr);
            return nullptr;
        }
        // fix #2419 (§9.8 / stdlib §2.15, v0.235): skip whitespace between the
        // "$case" colon and its value so ordinary/pretty-printed JSON spacing
        // (`"$case": "Both"`) is tolerated exactly like every other field-value
        // read in this file (e.g. bz_binder_read_string at line ~1560, which
        // skips ws before its own bz_jder_parse_string call). Without this,
        // bz_jder_parse_string_cps requires d->pos to sit exactly on the
        // opening '"' and spuriously raises "failed to read \"$case\" value"
        // on legally-formatted, RFC 8259-compliant JSON.
        bz_jder_skip_ws(d);
        void* disc = bz_jder_parse_string(d);
        if (!disc) {
            // fix #2432 (§9.8 / stdlib §2.15, v0.235): bz_jder_parse_string raises
            // internally on every malformed-input path; the one exception (OOM in
            // bz_jder_cps_to_bz_string) returns nullptr WITHOUT throwing, so the
            // manual bz_ser_throw below remains the correct safety net for THAT
            // case. Bail out only when an exception is already in flight
            // (Bug-A pattern, LSan-confirmed: an unknown \z escape in the "$case"
            // value leaked 122 bytes / 3 allocations behind the wrong message).
            if (bz_exc) return nullptr;
            bz_ser_throw("Choice deserialize: failed to read \"$case\" value (§9.8)",
                         nullptr);
            return nullptr;
        }
        bz_jder_skip_ws(d);
        if (d->pos < d->len && d->data[d->pos] == ',') ++d->pos;

        d->poly_preread_count = total_count - 1;
        d->poly_preread_active = 1;  // honour a zero remaining-field count (#2046)
        return disc;
    }

    // Binary path.
    uint32_t field_count = 0;
    if (!bz_binder_read_le32_impl(d, &field_count)) {
        bz_ser_throw("Choice deserialize: truncated binary object header (§9.8)",
                     nullptr);
        return nullptr;
    }
    if (field_count == 0) {
        bz_ser_throw("Choice deserialize: empty object, missing \"$case\" "
                     "discriminator (§9.8)", nullptr);
        return nullptr;
    }
    void* keyStr = bz_binder_read_bz_string(d);
    if (!keyStr) {
        bz_ser_throw("Choice deserialize: missing field name (§9.8)", nullptr);
        return nullptr;
    }
    const auto* kbz = static_cast<const bz_string_struct*>(keyStr);
    bool isCaseKey = (kbz->len == 5 && kbz->width == 1);
    if (isCaseKey) {
        const uint8_t* kd = static_cast<const uint8_t*>(kbz->data);
        isCaseKey = (kd[0]=='$' && kd[1]=='c' && kd[2]=='a' && kd[3]=='s' && kd[4]=='e');
    }
    bz_string_release(keyStr);
    if (!isCaseKey) {
        bz_ser_throw("Choice deserialize: object does not lead with the \"$case\" "
                     "discriminator (§9.8)", nullptr);
        return nullptr;
    }
    void* disc = bz_binder_read_bz_string(d);
    if (!disc) {
        bz_ser_throw("Choice deserialize: failed to read \"$case\" binary value (§9.8)",
                     nullptr);
        return nullptr;
    }
    d->poly_preread_count = (int32_t)(field_count - 1);
    d->poly_preread_active = 1;  // honour a zero remaining-field count (#2046)
    d->in_object = true;
    return disc;
}

// §2.15 / §9.8: the SerializationException vtable pointer registered by the
// codegen-generated main prologue (bz_ser_exc_vtable_init).  Initially null;
// set before any user code can throw.  bz_ser_throw uses this so runtime-
// internal throws produce a well-formed exception with a valid vtable, enabling
// emitIsA's catch dispatch to work correctly (it dereferences vtable[0]).
void* bz_ser_exc_vtable = nullptr;

void bz_ser_exc_vtable_init(void* vtable) {
    bz_ser_exc_vtable = vtable;
}

// bz_ser_throw: build a minimal SerializationException-like exception object
// and store it in the global bz_exc slot.  Uses bz_ser_exc_vtable (registered
// by the main prologue) so that the exception passes emitIsA's type check.
// The legacy `ser_exn_vtable` parameter is kept for ABI compatibility but
// ignored — the global is always used so runtime-internal JSON throws work.
void bz_ser_throw(const char* msg, void* /*ser_exn_vtable*/) {
    // §2.15 / #1033: build a SerializationException object (§31.4 layout) with a
    // PROPERLY REFCOUNTED message. The built-in exception disposer (bz$exDisposeMsg,
    // vtable slot 2) releases the owned Message@8 via bz_string_release on disposal
    // (#1033 / §31.1 / §28.9), so the message MUST be a normal refcounted bz_string
    // (rc = 1, independently-allocated data + header) — exactly like bz_scope_make_ioe
    // and lowerNew's +1 producer — NOT an rc=0 string packed into the same block.
    //
    // The earlier single-contiguous-block design (rc=0 embedded at offset 16) was
    // crafted for the former NO-OP disposer, where free(exc_obj) reclaimed the whole
    // block at once. Under the string-releasing disposer that embedded layout
    // CORRUPTS THE HEAP: bz_string_release sees rc=0 (not the sentinel), decrements
    // it to -1, and free()s the data pointer AND the header — both interior pointers
    // into the same block — then emitDisposeAndReclaim free()s the block. That
    // double-free crashed every deserialization "raises" path (malformed JSON, type
    // mismatch, unknown field, NaN/Infinity, depth limit, …). The exception object is
    // now calloc'd (BZ_EXC_OBJ_SIZE, message slot null); the disposer frees the
    // message, then free(obj) reclaims the exception block — balanced, no leak.
    void* obj = calloc(1, (size_t)BZ_EXC_OBJ_SIZE);
    memcpy(obj, &bz_ser_exc_vtable, sizeof(void*));  // slot 0 = vtable
    if (msg) {
        void* s = bz_make_string(msg, (int64_t)strlen(msg));  // rc = 1 (+1 transfer)
        memcpy((uint8_t*)obj + 8, &s, sizeof(void*));         // slot 1 = Message@8
    }

    extern thread_local void* bz_exc;
    bz_exc = obj;
}

// OwningBuffer<byte> helpers --------------------------------------------------

// Allocate an OwningBuffer<byte> containing a copy of `data[0..len)`.
// Returns bz_array_t* (elem_size 1).  Caller owns the result.
void* bz_owned_buffer_byte_alloc(const uint8_t* data, int64_t len) {
    void* arr = bz_array_alloc(1, len > 0 ? len : 1);
    if (data && len > 0) {
        auto* a = static_cast<bz_array_t*>(arr);
        memcpy(a->data, data, (size_t)len);
        a->count = len;
    }
    return arr;
}

// Return a pointer+length pair from an OwningBuffer<byte> (bz_array_t* with
// elem_size 1).  The span borrows from the buffer; the buffer must outlive it.
void bz_owned_buffer_get_span(void* bufp,
                               const uint8_t** data_out,
                               int64_t* len_out) {
    if (!bufp) { *data_out = nullptr; *len_out = 0; return; }
    auto* arr = static_cast<bz_array_t*>(bufp);
    *data_out = static_cast<const uint8_t*>(arr->data);
    *len_out  = arr->count;
}

} // extern "C" (binary serialization)
