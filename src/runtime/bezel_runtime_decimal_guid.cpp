// bezel_runtime_decimal_guid.cpp — decimal (128-bit) and Guid.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// decimal (§4.4 / stdlib §9.3) — 128-bit high-precision decimal type.
//
// Representation: bz_decimal_t = { uint32_t lo32, mid32, hi32, flags }
//   flags bit 31  = sign (1 = negative)
//   flags bits 16..23 = scale s (0..28): value = mantissa / 10^s
//   mantissa = (hi32 << 64) | (mid32 << 32) | lo32  (96-bit unsigned integer)
//
// All arithmetic follows .NET Decimal semantics (28–29 significant decimal
// digits).  OverflowException if the result cannot be represented (> MaxValue
// or needs scale > 28).  DivideByZeroException if the divisor is zero (§6.4).
//
// Implementation strategy: we carry the mantissa as __uint128_t internally.
// This is supported by clang++ on x86_64 (the only compiler used here per
// CLAUDE.md).  All internal arithmetic normalises the scale to the minimum
// possible (trailing zeros removed) before storing back.
// ---------------------------------------------------------------------------

// Note on exceptions:
// - DivideByZeroException: checked INLINE by the codegen before calling
//   bz_decimal_div / bz_decimal_mod (matching the integer division pattern).
//   The codegen inspects the three mantissa i32 fields and throws if all zero.
//   The runtime functions assume the divisor is non-zero.
// - OverflowException: raised via bz_set_exc(ove_vtable) into the bz_exc slot.
//   Each arithmetic function receives the OverflowException vtable as a
//   parameter (ove_vtable) and stores a minimal exception object in bz_exc.
//   The codegen caller calls emitPendingCheck() after every arithmetic call.
//   Pattern mirrors fix #634 / bz_channel_send / bz_ser_throw (§6.4).
// - FormatException: same mechanism, separate vtable (fe_vtable), used by
//   bz_decimal_parse_cstr for malformed input.

// Helper: allocate a built-in exception object {vtable*, ...} (§31.4 layout,
// message/causechain/paramname slots left null by calloc) and store it in bz_exc
// (the global pending-exception slot). The block size matches codegen's
// kExcObjSize so the base-Exception getters (CauseChain @ 24) read a valid slot.
void bz_set_exc(void* vtable) {
    if (!vtable) return;
    void** obj = static_cast<void**>(calloc(1, (size_t)BZ_EXC_OBJ_SIZE));
    memcpy(obj, &vtable, sizeof(void*));
    extern thread_local void* bz_exc;
    bz_exc = obj;
}


// Powers of ten as __uint128_t (indices 0..28).
static const __uint128_t kPow10[29] = {
    ((__uint128_t)1),                                    // 10^0
    ((__uint128_t)10),                                   // 10^1
    ((__uint128_t)100),                                  // 10^2
    ((__uint128_t)1000),                                 // 10^3
    ((__uint128_t)10000),                                // 10^4
    ((__uint128_t)100000),                               // 10^5
    ((__uint128_t)1000000),                              // 10^6
    ((__uint128_t)10000000),                             // 10^7
    ((__uint128_t)100000000),                            // 10^8
    ((__uint128_t)1000000000),                           // 10^9
    ((__uint128_t)10000000000ULL),                       // 10^10
    ((__uint128_t)100000000000ULL),                      // 10^11
    ((__uint128_t)1000000000000ULL),                     // 10^12
    ((__uint128_t)10000000000000ULL),                    // 10^13
    ((__uint128_t)100000000000000ULL),                   // 10^14
    ((__uint128_t)1000000000000000ULL),                  // 10^15
    ((__uint128_t)10000000000000000ULL),                 // 10^16
    ((__uint128_t)100000000000000000ULL),                // 10^17
    ((__uint128_t)1000000000000000000ULL),               // 10^18
    ((__uint128_t)10000000000000000000ULL),              // 10^19
    ((__uint128_t)10000000000000000000ULL) * 10,         // 10^20
    ((__uint128_t)10000000000000000000ULL) * 100,        // 10^21
    ((__uint128_t)10000000000000000000ULL) * 1000,       // 10^22
    ((__uint128_t)10000000000000000000ULL) * 10000,      // 10^23
    ((__uint128_t)10000000000000000000ULL) * 100000,     // 10^24
    ((__uint128_t)10000000000000000000ULL) * 1000000,    // 10^25
    ((__uint128_t)10000000000000000000ULL) * 10000000,   // 10^26
    ((__uint128_t)10000000000000000000ULL) * 100000000,  // 10^27
    ((__uint128_t)10000000000000000000ULL) * 1000000000, // 10^28
};

// Maximum mantissa for a 96-bit unsigned integer (2^96 - 1).
static const __uint128_t kMax96 =
    (((__uint128_t)0xFFFFFFFFULL << 64) |
     ((__uint128_t)0xFFFFFFFFULL << 32) |
      (__uint128_t)0xFFFFFFFFULL);

// MaxValue mantissa: 79228162514264337593543950335 = 10^29 - 1 rounded to 96 bits.
// Actually .NET MaxValue = 79228162514264337593543950335 with scale 0.
// In binary: 0xFFFFFFFF FFFFFFFF FFFFFFFF (all 96 bits set).
// This is the largest 96-bit number = 2^96 - 1.

// Helper: store mantissa + scale + sign back into a bz_decimal_t.
// Returns true on success; on OverflowException (§6.4) stores the exception
// object via bz_set_exc(ove_vtable) and returns false.
// per fix #521 / §6.4: overflow raises OverflowException, not abort().
static bool dec_store(bz_decimal_t* out, __uint128_t m, uint32_t s, int neg,
                      void* ove_vtable) {
    // Remove trailing decimal zeros to minimise scale.
    while (s > 0 && m % 10 == 0) { m /= 10; --s; }
    // If mantissa still overflows 96 bits, try to reduce by increasing the
    // scale (divides the value by 10, dropping trailing digits, rounding half-up).
    while (m > kMax96) {
        if (s == 0) {
            // §6.4: OverflowException — pathological case (value > MaxValue).
            bz_set_exc(ove_vtable);
            return false;
        }
        __uint128_t rem = m % 10;
        m = m / 10;
        if (rem >= 5) m += 1;  // round half-up
        --s;
        // Remove new trailing zeros.
        while (s > 0 && m % 10 == 0) { m /= 10; --s; }
    }
    if (m > kMax96) {
        // should not reach here — defensive overflow guard (§6.4).
        bz_set_exc(ove_vtable);
        return false;
    }
    if (m == 0) neg = 0;  // no negative zero per .NET
    out->lo32  = (uint32_t)(m & 0xFFFFFFFF);
    out->mid32 = (uint32_t)((m >> 32) & 0xFFFFFFFF);
    out->hi32  = (uint32_t)((m >> 64) & 0xFFFFFFFF);
    out->flags = ((uint32_t)neg << 31) | ((s & 0xFF) << 16);
    return true;
}

// Helper: align two decimals to the same scale, returning false if impossible.
// Sets *ma, *mb, *s to the aligned mantissas and common scale.
// If rescaling would overflow 96 bits for the larger one, we truncate the
// smaller scale's mantissa (losing trailing zeros is lossless; upscaling the
// larger is avoided).
static bool dec_align(__uint128_t ma, uint32_t sa,
                      __uint128_t mb, uint32_t sb,
                      __uint128_t* oA, __uint128_t* oB, uint32_t* os) {
    while (sa < sb) {
        // Need to multiply ma by 10 to match sb.
        if (ma > kMax96 / 10) {
            // ma would overflow: try dividing mb instead.
            __uint128_t rem = mb % 10;
            mb /= 10;
            if (rem >= 5) mb += 1;
            --sb;
            continue;
        }
        ma *= 10; ++sa;
    }
    while (sb < sa) {
        if (mb > kMax96 / 10) {
            __uint128_t rem = ma % 10;
            ma /= 10;
            if (rem >= 5) ma += 1;
            --sa;
            continue;
        }
        mb *= 10; ++sb;
    }
    *oA = ma; *oB = mb; *os = sa;
    return true;
}

// §4.4 / §6.4: decimal + decimal
// per fix #521 / §6.4: ove_vtable is the OverflowException vtable; stored in
// bz_exc on overflow so codegen's emitPendingCheck() unwinds.
extern "C" void bz_decimal_add(const bz_decimal_t* a, const bz_decimal_t* b,
                                bz_decimal_t* out, void* ove_vtable) {
    __uint128_t ma = dec_mantissa(a), mb = dec_mantissa(b);
    uint32_t sa = dec_scale(a), sb = dec_scale(b);
    int signa = dec_sign(a), signb = dec_sign(b);
    __uint128_t ra, rb; uint32_t rs;
    dec_align(ma, sa, mb, sb, &ra, &rb, &rs);
    int rsign;
    __uint128_t rm;
    if (signa == signb) {
        rm = ra + rb; rsign = signa;
        // carry: may overflow 96 bits, dec_store will reduce scale.
    } else {
        if (ra >= rb) { rm = ra - rb; rsign = signa; }
        else          { rm = rb - ra; rsign = signb; }
    }
    dec_store(out, rm, rs, rsign, ove_vtable);
}

// §4.4 / §6.4: decimal - decimal
// per fix #521 / §6.4: ove_vtable flows through to bz_decimal_add/dec_store.
extern "C" void bz_decimal_sub(const bz_decimal_t* a, const bz_decimal_t* b,
                                bz_decimal_t* out, void* ove_vtable) {
    // a - b = a + (-b)
    bz_decimal_t nb = *b;
    nb.flags ^= (1u << 31);  // flip sign
    if (dec_mantissa(&nb) == 0) nb.flags &= ~(1u << 31);  // no -0
    bz_decimal_add(a, &nb, out, ove_vtable);
}

// §4.4 / §6.4: decimal * decimal
// per fix #521 / §6.4: ove_vtable flows to dec_store for overflow signalling.
extern "C" void bz_decimal_mul(const bz_decimal_t* a, const bz_decimal_t* b,
                                bz_decimal_t* out, void* ove_vtable) {
    __uint128_t ma = dec_mantissa(a), mb = dec_mantissa(b);
    uint32_t sa = dec_scale(a) + dec_scale(b);
    int rsign = dec_sign(a) ^ dec_sign(b);
    // Product may be up to 192 bits; use 256-bit arithmetic via splitting.
    // Split mb = mb_hi * 2^64 + mb_lo; a * b = a*mb_hi*2^64 + a*mb_lo.
    __uint128_t mb_lo = (uint64_t)mb;
    __uint128_t mb_hi = (uint64_t)(mb >> 64);
    // Use unsigned __int128 arithmetic. The product of two 96-bit numbers
    // is at most 192 bits.  We compute it as: product = ma * mb,
    // working in 256 bits by splitting.
    // For simplicity: split ma into 32-bit limbs.
    uint64_t a0 = (uint32_t)(ma),          a1 = (uint32_t)(ma >> 32),
             a2 = (uint32_t)(ma >> 64);
    uint64_t b0 = (uint32_t)(mb),          b1 = (uint32_t)(mb >> 32),
             b2 = (uint32_t)(mb >> 64);
    // 6x6 32-bit partial products, sum into 5 64-bit accumulators.
    uint64_t c[6] = {0, 0, 0, 0, 0, 0};
    // Each partial: a_i * b_j -> add at position i+j.
    auto add64 = [&](int pos, uint64_t v) {
        while (v && pos < 6) {
            uint64_t old = c[pos];
            c[pos] += v;
            v = (c[pos] < old) ? 1 : 0;
            ++pos;
        }
    };
    add64(0, a0 * b0);
    add64(1, a0 * b1); add64(1, a1 * b0);
    add64(2, a0 * b2); add64(2, a1 * b1); add64(2, a2 * b0);
    add64(3, a1 * b2); add64(3, a2 * b1);
    add64(4, a2 * b2);

    // Reassemble into __uint128_t pairs: lo128 (c[0..3]) and hi128 (c[4..5]).
    __uint128_t lo128 = ((__uint128_t)c[1] << 32) | c[0];
    lo128 |= ((__uint128_t)(c[2] & 0xFFFFFFFF) << 64);
    lo128 |= ((__uint128_t)(c[2] >> 32) << 96);
    // Actually, let's recompute more carefully with 32-bit limbs.
    // We have 6 limbs of 32 bits (indices 0..5 × 32 = 192 bits total).
    // Reconstruct the 192-bit value.
    // Limb i holds bits [i*32 .. i*32+63] worth (up to 64-bit partial sums,
    // but we need to propagate carries properly).
    // Propagate carries:
    for (int i = 0; i < 5; ++i) {
        c[i+1] += c[i] >> 32;
        c[i] &= 0xFFFFFFFF;
    }
    // Now c[0..5] are each 32-bit values representing the 192-bit product.
    // Reconstruct as __uint128_t lo + hi.
    __uint128_t product_lo = (((__uint128_t)c[3] << 96) |
                               ((__uint128_t)c[2] << 64) |
                               ((__uint128_t)c[1] << 32) |
                                (__uint128_t)c[0]);
    uint64_t product_hi_words = ((uint64_t)c[5] << 32) | c[4];
    // product = product_hi_words * 2^128 + product_lo
    // We need to reduce this into a 96-bit mantissa by dividing by powers
    // of 10 (adjusting scale down). If product_hi_words != 0, we must
    // scale down until the result fits in 96 bits.

    // Reconstruct as a scaled value: try to reduce scale while trimming
    // the product to 96 bits.
    // Simple approach: divide by 10^k until it fits, reducing sa by k.
    // Use 192-bit division by 10 iteratively.
    // product / 10: (hi * 2^128 + lo) / 10
    while (product_hi_words != 0 || product_lo > kMax96) {
        // Divide 192-bit value by 10.
        // hi_words / 10 = q_hi, remainder r_hi
        uint64_t q_hi = product_hi_words / 10;
        uint64_t r_hi = product_hi_words % 10;
        // (r_hi * 2^128 + lo) / 10 = lo / 10 + r_hi * (2^128 / 10)
        // 2^128 / 10 = 34028236692093846346337460743176821145 + rem
        // This is complex; use a simpler iterative approach.
        // Divide the 192-bit number by 10:
        // Let N = hi * 2^128 + lo.
        // q = N / 10, r = N mod 10.
        // We compute r first, then divide.
        // r = (product_hi_words * (2^128 mod 10) + product_lo mod 10) mod 10
        // 2^128 mod 10 = 6 (since 2^4=16≡6 and cycles with period 4; 128/4=32, rem=0 → 2^128≡6 mod 10)
        // Actually: 2^1=2, 2^2=4, 2^3=8, 2^4=6(mod10), 2^5=2... period 4.
        // 128 mod 4 = 0 → 2^128 ≡ 2^4 ≡ 6 (mod 10).
        // remainder = (product_hi_words * 6 + (product_lo % 10)) % 10
        // This only gives us the last digit, but we need to do the full division.
        // Use 64-bit chunks:
        // Treat as 6 × 32-bit limbs and do long division.
        uint64_t limbs[6];
        for (int i = 0; i < 4; ++i) limbs[i] = (product_lo >> (32*i)) & 0xFFFFFFFF;
        limbs[4] = product_hi_words & 0xFFFFFFFF;
        limbs[5] = product_hi_words >> 32;
        uint64_t rem = 0;
        for (int i = 5; i >= 0; --i) {
            uint64_t cur = (rem << 32) | limbs[i];
            limbs[i] = cur / 10;
            rem = cur % 10;
        }
        // Reconstruct.
        product_hi_words = ((uint64_t)limbs[5] << 32) | limbs[4];
        product_lo = (((__uint128_t)limbs[3] << 96) |
                      ((__uint128_t)limbs[2] << 64) |
                      ((__uint128_t)limbs[1] << 32) |
                       (__uint128_t)limbs[0]);
        // If sa > 0, reduce scale (divides by 10, legal rounding).
        // If sa == 0, the product genuinely exceeds MaxValue — OverflowException.
        if (sa > 0) { --sa; }
        else {
            // §6.4: OverflowException — product > MaxValue with no remaining
            // scale to reduce.  Cannot represent the result.
            bz_set_exc(ove_vtable);
            return;
        }
        if (rem >= 5) {
            // Round: add 1 to the truncated result.
            __uint128_t old = product_lo;
            if (product_hi_words == 0) {
                product_lo += 1;
            } else {
                product_lo += 1;
                if (product_lo < old) product_hi_words += 1;
            }
        }
    }
    dec_store(out, product_lo, sa, rsign, ove_vtable);
}

// §4.4 / §6.4: decimal / decimal.
// PRE: divisor is non-zero (codegen checks via emitGuardThrow before calling).
// per fix #521 / §6.4: ove_vtable flows to dec_store for overflow signalling.
extern "C" void bz_decimal_div(const bz_decimal_t* a, const bz_decimal_t* b,
                                bz_decimal_t* out, void* ove_vtable) {
    __uint128_t mb = dec_mantissa(b);
    if (mb == 0) { abort(); return; }  // precondition violated — codegen guards this
    __uint128_t ma = dec_mantissa(a);
    uint32_t sa = dec_scale(a), sb = dec_scale(b);
    int rsign = dec_sign(a) ^ dec_sign(b);
    // Compute quotient: (ma / 10^sa) / (mb / 10^sb) = (ma / mb) * 10^(sb-sa)
    // We want: result_mantissa = ma * 10^extra / mb, result_scale = sa - sb + extra
    // Choose extra to get as many significant digits as possible (up to 28).
    // Strategy: scale up ma by 10^k (up to k=28) before dividing, to get
    // good precision. Use the largest k such that ma * 10^k fits in 128 bits.
    uint32_t extra = 0;
    // We can scale up ma as long as ma * 10 <= MaxUint128 (but we use 96-bit
    // precision; scale up to kMax96).
    while (extra + sa < 29 && ma <= kMax96 / 10) {
        ma *= 10; ++extra;
    }
    __uint128_t q = ma / mb;
    // Scale: sa + extra (from ma) - sb (denominator's scale) → net scale.
    int32_t rs_signed = (int32_t)(sa + extra) - (int32_t)sb;
    if (rs_signed < 0) {
        // Result scale is negative: multiply q by 10^|rs| to compensate.
        while (rs_signed < 0 && q <= kMax96 / 10) { q *= 10; ++rs_signed; }
        if (rs_signed < 0) { dec_store(out, q, 0, rsign, ove_vtable); return; }
    }
    uint32_t rs = (uint32_t)rs_signed;
    dec_store(out, q, rs, rsign, ove_vtable);
}

// §4.4 / §6.4: decimal mod decimal.
// PRE: divisor is non-zero (codegen checks via emitGuardThrow before calling).
// per fix #521 / §6.4: ove_vtable flows to dec_store for overflow signalling.
extern "C" void bz_decimal_mod(const bz_decimal_t* a, const bz_decimal_t* b,
                                bz_decimal_t* out, void* ove_vtable) {
    __uint128_t mb = dec_mantissa(b);
    if (mb == 0) { abort(); return; }  // precondition violated — codegen guards this
    __uint128_t ma = dec_mantissa(a);
    uint32_t sa = dec_scale(a), sb = dec_scale(b);
    int signa = dec_sign(a);
    // §6.1 dividend-sign rule: result has the sign of the dividend.
    // Align to common scale.
    __uint128_t ra, rb; uint32_t rs;
    dec_align(ma, sa, mb, sb, &ra, &rb, &rs);
    if (rb == 0) { abort(); return; }  // shouldn't happen given non-zero divisor
    __uint128_t q = ra / rb;
    __uint128_t rem = ra - q * rb;
    dec_store(out, rem, rs, signa, ove_vtable);
}

// Negation.
extern "C" void bz_decimal_neg(const bz_decimal_t* a, bz_decimal_t* out) {
    *out = *a;
    if (dec_mantissa(a) != 0)
        out->flags ^= (1u << 31);
}

// §8.3: comparison — returns negative/zero/positive.
extern "C" int32_t bz_decimal_cmp(const bz_decimal_t* a, const bz_decimal_t* b) {
    int signa = dec_sign(a), signb = dec_sign(b);
    __uint128_t ma = dec_mantissa(a), mb = dec_mantissa(b);
    // Both zero → equal.
    if (ma == 0 && mb == 0) return 0;
    // Different signs → sign determines order.
    if (signa != signb) return signa ? -1 : 1;
    // Same sign — align and compare mantissas.
    __uint128_t ra, rb; uint32_t rs;
    dec_align(ma, dec_scale(a), mb, dec_scale(b), &ra, &rb, &rs);
    int32_t cmp = (ra < rb) ? -1 : (ra > rb) ? 1 : 0;
    return signa ? -cmp : cmp;  // negative numbers: larger mantissa = smaller value
}

// fix #685 / §4.4 / §8.3: hash a decimal value consistently with bz_decimal_cmp.
// Two decimals that compare equal (e.g. 1.0m and 1.00m) must hash identically.
// Strategy: normalize by stripping trailing decimal zeros — divide the mantissa
// by 10 while it divides evenly and decrement the scale — then fold the canonical
// (sign, normalized mantissa) into an i32.  Any zero value (mantissa = 0)
// always returns 0 regardless of scale or sign bit.
extern "C" int32_t bz_decimal_hash(const bz_decimal_t* a) {
    __uint128_t m = dec_mantissa(a);
    uint32_t s    = dec_scale(a);
    int neg       = dec_sign(a);
    // All-zero mantissa: canonical zero (independent of scale and sign).
    if (m == 0) return 0;
    // Normalize: strip trailing decimal zeros by dividing m by 10 while
    // divisible, decrementing s in lock-step.
    while (s > 0 && m % 10 == 0) { m /= 10; --s; }
    // FNV-style fold of (sign, scale, mantissa lo64, mantissa hi64) → i32.
    // We use the low 96 bits of the mantissa (hi32 is the top word; in practice
    // a Bezel decimal fits within 96 bits).
    uint32_t lo32 = (uint32_t)( m        & 0xFFFFFFFFu);
    uint32_t md32 = (uint32_t)((m >> 32) & 0xFFFFFFFFu);
    uint32_t hi32 = (uint32_t)((m >> 64) & 0xFFFFFFFFu);
    uint32_t h    = 17u;
    h = h * 31u + lo32;
    h = h * 31u + md32;
    h = h * 31u + hi32;
    h = h * 31u + s;
    if (neg) h ^= 0x80000000u;
    return (int32_t)h;
}

// Conversion from int64/int32 (implicit per §5.1).
extern "C" void bz_decimal_from_int64(int64_t v, bz_decimal_t* out) {
    int neg = (v < 0);
    __uint128_t m = neg ? (v == INT64_MIN ? ((__uint128_t)1 << 63) : (__uint128_t)(-v))
                        : (__uint128_t)v;
    // int64 mantissa always fits in 96 bits (int64_max < 2^63 < 2^96); no overflow.
    dec_store(out, m, 0, neg, nullptr);
}

extern "C" void bz_decimal_from_int32(int32_t v, bz_decimal_t* out) {
    bz_decimal_from_int64((int64_t)v, out);
}

// Conversion from an UNSIGNED 64-bit integer (#743): a ulong above
// long.MaxValue must NOT be reinterpreted as a negative int64. The full
// uint64 range (< 2^64) fits in the 96-bit mantissa, so no overflow.
extern "C" void bz_decimal_from_uint64(uint64_t v, bz_decimal_t* out) {
    dec_store(out, (__uint128_t)v, 0, /*neg=*/0, nullptr);
}

// Conversion from double (explicit per §5.2).
// per fix #521 / §6.4: ove_vtable is stored in bz_exc on overflow; callers
// call emitPendingCheck() after this function.
extern "C" void bz_decimal_from_double(double v, bz_decimal_t* out,
                                        void* ove_vtable) {
    // Use snprintf to get the decimal representation.
    if (v != v || v > 1e29 || v < -1e29) {
        // §6.4: OverflowException — value out of decimal range.
        bz_set_exc(ove_vtable);
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.15g", v);
    // ove_vtable also covers the (very unlikely) case where the string
    // produced by snprintf overflows (e.g. 1e29 edge case).
    bz_decimal_parse_cstr(buf, out, ove_vtable, nullptr);
}

// Conversion to double (explicit per §5.2).
extern "C" double bz_decimal_to_double(const bz_decimal_t* a) {
    __uint128_t m = dec_mantissa(a);
    uint32_t s = dec_scale(a);
    double result = 0.0;
    // Convert mantissa to double then divide by 10^scale.
    // Split m into hi64 and lo64 to avoid precision loss.
    uint64_t m_hi = (uint64_t)(m >> 64);
    uint64_t m_lo = (uint64_t)(m);
    result = (double)m_hi * 18446744073709551616.0 + (double)m_lo;
    if (s > 0) {
        static const double pow10d[29] = {
            1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000,
            1000000000, 10000000000.0, 100000000000.0, 1000000000000.0,
            10000000000000.0, 100000000000000.0, 1000000000000000.0,
            10000000000000000.0, 100000000000000000.0, 1000000000000000000.0,
            10000000000000000000.0, 1e20, 1e21, 1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28
        };
        result /= pow10d[s];
    }
    return dec_sign(a) ? -result : result;
}

// Conversion to int64 (explicit per §5.2).
// §5.2: truncates toward zero; raises OverflowException when the truncated
// integer value does not fit in int64.
extern "C" int64_t bz_decimal_to_int64(const bz_decimal_t* a, void* ove_vtable) {
    // Compute |mantissa| / 10^scale (truncation toward zero).
    __uint128_t m = dec_mantissa(a);
    uint32_t s = dec_scale(a);
    // Divide mantissa by 10^scale.
    static const __uint128_t pow10_128[29] = {
        (uint64_t)1, (uint64_t)10, (uint64_t)100, (uint64_t)1000,
        (uint64_t)10000, (uint64_t)100000, (uint64_t)1000000,
        (uint64_t)10000000, (uint64_t)100000000, (uint64_t)1000000000,
        (uint64_t)10000000000ULL, (uint64_t)100000000000ULL,
        (uint64_t)1000000000000ULL, (uint64_t)10000000000000ULL,
        (uint64_t)100000000000000ULL, (uint64_t)1000000000000000ULL,
        (uint64_t)10000000000000000ULL, (uint64_t)100000000000000000ULL,
        (uint64_t)1000000000000000000ULL,
        (__uint128_t)10000000000000000000ULL,
        (__uint128_t)10000000000000000000ULL * 10,
        (__uint128_t)10000000000000000000ULL * 100,
        (__uint128_t)10000000000000000000ULL * 1000,
        (__uint128_t)10000000000000000000ULL * 10000,
        (__uint128_t)10000000000000000000ULL * 100000,
        (__uint128_t)10000000000000000000ULL * 1000000,
        (__uint128_t)10000000000000000000ULL * 10000000,
        (__uint128_t)10000000000000000000ULL * 100000000,
        (__uint128_t)10000000000000000000ULL * 1000000000,
    };
    if (s >= 29) {
        // Any decimal with scale >= 29 has integer part = 0.
        return 0;
    }
    __uint128_t trunc = m / pow10_128[s];
    // Overflow check: truncated value must fit in int64.
    if (trunc > (__uint128_t)(uint64_t)INT64_MAX) {
        bz_set_exc(ove_vtable);
        return 0;
    }
    int64_t v = (int64_t)trunc;
    return dec_sign(a) ? -v : v;
}

// Conversion to int32 (explicit per §5.2).
// §5.2: truncates toward zero; raises OverflowException when the truncated
// value is outside int32 range.
extern "C" int32_t bz_decimal_to_int32(const bz_decimal_t* a, void* ove_vtable) {
    int64_t v = bz_decimal_to_int64(a, ove_vtable);
    if (bz_exc) return 0;  // OverflowException already set by to_int64
    // Narrowing check: value must fit in int32.
    if (v < (int64_t)INT32_MIN || v > (int64_t)INT32_MAX) {
        bz_set_exc(ove_vtable);
        return 0;
    }
    return (int32_t)v;
}

// Conversion to uint64 (explicit per §5.2, fix #4785).
// §5.2: truncates toward zero; raises OverflowException when the truncated
// value does not fit in uint64 — i.e. when the source decimal is negative
// OR its magnitude exceeds 2^64-1.  Decimal.MaxValue
// (79228162514264337593543950335 ≈ 7.9e28) is well above uint64.MaxValue
// (18446744073709551615 ≈ 1.8e19), so the upper-bound check is load-bearing
// here and was the silently-wrong-rejection root cause of #4785: the codegen
// `ulong(decimal)` arm at LlvmBackend_Exprs2.cpp:9825-9833 reused the SIGNED
// `bz_decimal_to_int64` helper, which enforces `trunc <= (uint64_t)INT64_MAX`
// and rejects every value in (int64.MaxValue, uint64.MaxValue] — half of
// `ulong`'s range.  A NEGATIVE decimal also makes this helper raise; the
// pre-fix signed helper silently converted -1m to 1 (and downstream, the
// ulong conversion reinterpreted that as ulong(1)), so this side of the bug
// is fixed structurally here too.
extern "C" uint64_t bz_decimal_to_uint64(const bz_decimal_t* a, void* ove_vtable) {
    // Same truncation logic as bz_decimal_to_int64 (the decimal-to-int helper
    // shares the truncation math because the only target-specific bits are
    // the upper-bound check and the sign application); we duplicate the
    // mantissa / scale / pow10 extraction rather than add an out-parameter
    // to the signed helper, mirroring the from-side pattern (the int64 and
    // uint64 FROM helpers are also separate — bz_decimal_from_int64 /
    // bz_decimal_from_uint64 — per fix #743).  Duplication is the smaller
    // refactor: threading a signedness flag through would touch every caller.
    __uint128_t m = dec_mantissa(a);
    uint32_t s = dec_scale(a);
    static const __uint128_t pow10_128[29] = {
        (uint64_t)1, (uint64_t)10, (uint64_t)100, (uint64_t)1000,
        (uint64_t)10000, (uint64_t)100000, (uint64_t)1000000,
        (uint64_t)10000000, (uint64_t)100000000, (uint64_t)1000000000,
        (uint64_t)10000000000ULL, (uint64_t)100000000000ULL,
        (uint64_t)1000000000000ULL, (uint64_t)10000000000000ULL,
        (uint64_t)100000000000000ULL, (uint64_t)1000000000000000ULL,
        (uint64_t)10000000000000000ULL, (uint64_t)100000000000000000ULL,
        (uint64_t)1000000000000000000ULL,
        (__uint128_t)10000000000000000000ULL,
        (__uint128_t)10000000000000000000ULL * 10,
        (__uint128_t)10000000000000000000ULL * 100,
        (__uint128_t)10000000000000000000ULL * 1000,
        (__uint128_t)10000000000000000000ULL * 10000,
        (__uint128_t)10000000000000000000ULL * 100000,
        (__uint128_t)10000000000000000000ULL * 1000000,
        (__uint128_t)10000000000000000000ULL * 10000000,
        (__uint128_t)10000000000000000000ULL * 100000000,
        (__uint128_t)10000000000000000000ULL * 1000000000,
    };
    if (s >= 29) {
        // Any decimal with scale >= 29 has integer part = 0 — a valid ulong(0).
        return 0;
    }
    __uint128_t trunc = m / pow10_128[s];
    // Range check: the unsigned target REJECTS negatives (the sign bit of a
    // decimal is separate from the mantissa, so a negative decimal with
    // |mantissa| < 2^63 would have fit in int64 but does NOT fit in uint64).
    if (dec_sign(a) || trunc > (__uint128_t)UINT64_MAX) {
        bz_set_exc(ove_vtable);
        return 0;
    }
    return (uint64_t)trunc;
}

// A code-point sequence to parse (fix #2273).  The parser reads by INDEX over
// an explicit length, never by scanning for a NUL: the former `char buf[64]`
// copies in bz_decimal_parse / bz_decimal_try_parse truncated any input past 63
// bytes and parsed the prefix, returning a wrong value with no exception (e.g.
// leading zeros past 63 characters) — the silent-truncation class fix #1829 set
// out to close.  `width` is a `string`'s implementation-defined storage width
// (language spec §4.6: 1, 2, or 4 bytes per code point); reading through it
// keeps the parser correct for any width instead of byte-scanning a wide
// string's first bytes, and makes an embedded NUL an ordinary non-digit
// (FormatException) rather than a silent end-of-input.
struct dec_src {
    const void* data;
    int64_t     len;
    uint8_t     width;
};
static inline uint32_t dec_src_at(const dec_src& s, int64_t i) {
    if (s.width == 2) return ((const uint16_t*)s.data)[i];
    if (s.width == 4) return ((const uint32_t*)s.data)[i];
    return ((const uint8_t*)s.data)[i];
}

// The shared decimal parse engine.  `allow_exp` selects which of the project's
// TWO decimal grammars applies — they differ, and the difference is normative:
//
//   true  — the decimal LITERAL grammar (language spec §18.1 and Appendix A:
//           `FloatLiteral ::= DigitGroup "." DigitGroup Exponent? FloatSuffix?
//                           | DigitGroup Exponent FloatSuffix?`), which admits
//           scientific notation (`1.23e2m`).  Used by the compile-time literal
//           folder (src/codegen/LlvmBackend_Exprs.cpp, src/sema/
//           ExprTypecheck_Exprs.cpp) and by bz_decimal_from_double, whose
//           "%.15g" rendering emits exponent form.
//   false — the stdlib §9.3 `Decimal.Parse` grammar: "an optional leading sign
//           (+/-) followed by decimal digits with at most one `.`" — NO
//           exponent.  Used by Decimal.Parse/TryParse and, by §2.3's "same
//           contract as the §9 per-type Parse family", Convert.ToDecimal/
//           TryToDecimal.
//
// FormatException (malformed) is stored via fe_vtable, OverflowException
// (out-of-range) via ove_vtable; either may be null (compile-time folding has
// no vtables).  On failure bz_exc is set and *out is not written.
static void dec_parse_src(const dec_src& src, bz_decimal_t* out,
                          void* ove_vtable, void* fe_vtable, bool allow_exp) {
    int64_t i = 0;
    // Skip optional sign.
    int neg = 0;
    if (i < src.len) {
        uint32_t c0 = dec_src_at(src, 0);
        if (c0 == '-') { neg = 1; ++i; }
        else if (c0 == '+') { ++i; }
    }
    // Collect digits, find decimal point.
    __uint128_t m = 0;
    uint32_t sc = 0;
    bool in_frac = false;
    bool any_digit = false;
    // §4.4 / §9.3 (fix #1829): count integer-part digits the full 96-bit
    // mantissa could not store.  Each is a factor of 10 in the true magnitude
    // the stored mantissa loses; it must be absorbed by scale (a '.' fraction
    // or a negative exponent) or the value is out of range — settled below.
    uint32_t dropped_int = 0;
    for (; i < src.len; ++i) {
        uint32_t c = dec_src_at(src, i);
        if (c == '.') {
            // §9.3 (fix #2273): "at most one `.`".  Pre-fix a second '.' merely
            // re-entered the fraction, so Parse("1.2.3") returned 1.23 while
            // TryParse's separate validator rejected it — a direct violation of
            // §9.3's "TryParse returns false in every case where Parse would
            // raise" in the *other* direction.
            if (in_frac) { bz_set_exc(fe_vtable); return; }
            in_frac = true;
            continue;
        }
        if (c == 'e' || c == 'E') {
            // Scientific notation — admitted for literals, declined for §9.3.
            if (!allow_exp) { bz_set_exc(fe_vtable); return; }
            ++i;
            int exp_neg = 0;
            if (i < src.len) {
                uint32_t sgn = dec_src_at(src, i);
                if (sgn == '-') { exp_neg = 1; ++i; }
                else if (sgn == '+') { ++i; }
            }
            int exp = 0;
            bool exp_digit = false;
            for (; i < src.len; ++i) {
                uint32_t d = dec_src_at(src, i);
                // Any non-digit here (including trailing junk after the
                // exponent) is malformed; pre-fix the loop simply stopped and
                // the remainder of the string was ignored.
                if (d < '0' || d > '9') { bz_set_exc(fe_vtable); return; }
                exp_digit = true;
                // Saturate: |exp| >= 1000 is out of range for any mantissa, and
                // accumulating it unbounded is signed-overflow UB.
                if (exp < 1000) exp = exp * 10 + (int)(d - '0');
            }
            if (!exp_digit) { bz_set_exc(fe_vtable); return; }
            if (exp_neg) sc += (uint32_t)exp;
            else {
                if ((int)sc - exp < 0) {
                    // Multiply by 10^(exp-sc): shift mantissa left.
                    int shift = exp - (int)sc;
                    for (int k = 0; k < shift; ++k) {
                        if (m > kMax96 / 10) {
                            // §6.4: OverflowException — mantissa too large for decimal range.
                            bz_set_exc(ove_vtable);
                            return;
                        }
                        m *= 10;
                    }
                    sc = 0;
                } else {
                    sc -= (uint32_t)exp;
                }
            }
            break;
        }
        if (c < '0' || c > '9') {
            // FormatException — unexpected character in decimal string.
            bz_set_exc(fe_vtable);
            return;
        }
        any_digit = true;
        if (m <= kMax96 / 10) {
            m = m * 10 + (uint32_t)(c - '0');
            // §4.4 / §9.3 (fix #2273): the scale counts the fractional digits
            // the mantissa actually HOLDS.  Pre-fix `++sc` ran before this
            // guard, so a fractional digit that was dropped still divided the
            // result by 10 — Parse("12.3456789012345678901234567890") returned
            // 1.2345678901234567890123456789, silently 10x too small — and the
            // inflated scale then absorbed dropped integer digits the value
            // could not actually pay for, defeating the #1829 range check.
            if (in_frac) ++sc;
        } else if (!in_frac) {
            // §4.4 / §9.3 (fix #1829): the 96-bit mantissa is full and this
            // digit is in the integer part, so it cannot be stored.  Count it:
            // it is a factor of 10 in the magnitude that must be paid for by
            // scale or reported as out of range.  (Fractional digits beyond
            // the 29th significant are dropped: they change neither the
            // magnitude nor the scale, only the precision.)
            ++dropped_int;
        }
    }
    if (!any_digit) {
        // FormatException — no digits found.
        bz_set_exc(fe_vtable);
        return;
    }
    // §4.4 / §9.3 (fix #1829): settle dropped integer-part digits against the
    // accumulated scale.  Each dropped digit is a factor of 10 in the magnitude
    // the stored mantissa lost; the scale (stored fractional digits + any
    // negative exponent) absorbs it by the same factor, so consuming
    // `dropped_int` from the scale reconstructs the magnitude exactly (the
    // dropped digits become fractional and are rounded by dec_store).  If the
    // scale cannot absorb them, the value's magnitude exceeds
    // `decimal.MaxValue` — raise OverflowException, never silently truncate.
    // With #2273's scale accounting this test is sound: a digit is dropped only
    // once m > kMax96/10, so a residual factor of 10 puts the value above
    // kMax96 = MaxValue's mantissa.
    if (dropped_int > 0) {
        if (dropped_int > sc) {
            // §6.4: OverflowException — magnitude beyond decimal's range.
            bz_set_exc(ove_vtable);
            return;
        }
        sc -= dropped_int;
    }
    dec_store(out, m, sc, neg, ove_vtable);
}

// Parse from C string (null-terminated) — the LITERAL-grammar entry (§18.1).
// per fix #521 / §6.4: FormatException (malformed) stored via fe_vtable;
// OverflowException (out-of-range) stored via ove_vtable.  Either may be
// nullptr (used during compile-time initialisation where no vtable exists).
// On failure bz_exc is set and the function returns without writing *out.
extern "C" void bz_decimal_parse_cstr(const char* s, bz_decimal_t* out,
                                       void* ove_vtable, void* fe_vtable) {
    if (!s) { bz_set_exc(fe_vtable); return; }  // FormatException — null input
    dec_src src{ s, (int64_t)strlen(s), 1 };
    dec_parse_src(src, out, ove_vtable, fe_vtable, /*allow_exp=*/true);
}

// Parse from bz_string* (stdlib §9.3 Decimal.Parse; §2.3 Convert.ToDecimal).
// The layout is bezel_runtime_internal.hpp's bz_string_hdr — { char* data,
// int64_t len, uint8_t width, int64_t rc } — whose `width` is language spec
// §4.6's implementation-defined per-string storage width.
// per fix #521: raises FormatException (malformed) or OverflowException
// (out-of-range) via bz_exc; codegen calls emitPendingCheck() after this call.
// per fix #2273: parses the string's full length (no 63-byte truncation) under
// §9.3's grammar, which does not admit an exponent.
extern "C" void bz_decimal_parse(const void* str, bz_decimal_t* out,
                                  void* ove_vtable, void* fe_vtable) {
    if (!str) {
        // Null string argument — raise FormatException per §9.3.
        bz_set_exc(fe_vtable);
        return;
    }
    const auto* bs = (const bz_string_hdr*)str;
    if (bs->len <= 0 || !bs->data) {
        // Empty (or malformed) string — no digits, FormatException per §9.3.
        bz_set_exc(fe_vtable);
        return;
    }
    dec_src src{ bs->data, bs->len, bs->width };
    dec_parse_src(src, out, ove_vtable, fe_vtable, /*allow_exp=*/false);
}

// TryParse — returns 0 on failure (no throw), 1 on success.
// stdlib §9.3: "TryParse returns false in every case where Parse would raise";
// stdlib §2.3 (Convert.TryToDecimal, same runtime entry): "the `out` parameter
// receives the default value".
// per fix #2273: runs the PARSER itself behind sentinel vtables rather than a
// second, hand-maintained shape validator.  The duplicated validator had
// already drifted from the parser in both directions — it rejected "1.2.3"
// that Parse accepted, and admitted the exponent form §9.3 forbids — so the
// §9.3 contract can only be guaranteed structurally, by sharing one grammar.
extern "C" int32_t bz_decimal_try_parse(const void* str, bz_decimal_t* out) {
    // §2.3: the out slot receives default(decimal) = 0m on failure.  Pre-fix
    // the failure paths returned without writing it, and codegen stores the
    // (undef) slot into the caller's variable unconditionally.
    out->lo32 = 0; out->mid32 = 0; out->hi32 = 0; out->flags = 0;
    if (!str) return 0;
    // Non-null sentinel vtables so every failure the parser can report —
    // FormatException and OverflowException alike — is observable here as a
    // bz_exc store (fix #1829: a null vtable silently swallowed the overflow
    // via bz_set_exc's null no-op and returned success with a truncated value).
    unsigned char sentinel = 0;
    extern thread_local void* bz_exc;
    void* prev_exc = bz_exc;
    bz_exc = nullptr;
    bz_decimal_parse(str, out, &sentinel, &sentinel);
    if (bz_exc) {
        // Malformed or out of range — return 0 (no exception raised).  Discard
        // the internally set exc and restore the caller's exception state.
        free(bz_exc);
        bz_exc = prev_exc;
        out->lo32 = 0; out->mid32 = 0; out->hi32 = 0; out->flags = 0;
        return 0;
    }
    bz_exc = prev_exc;
    return 1;
}

// ToString (stdlib §9.3 Decimal.ToString()) — returns new bz_string* (rc=1).
// Layout mirrors bz_string_hdr defined in the Guid section below.
extern "C" void* bz_decimal_to_string(const bz_decimal_t* a) {
    __uint128_t m = dec_mantissa(a);
    uint32_t s = dec_scale(a);
    int neg = dec_sign(a);
    // Convert mantissa to decimal string via repeated division.
    char digits[32]; // 96-bit number has at most 29 decimal digits.
    int nd = 0;
    if (m == 0) { digits[0] = '0'; nd = 1; }
    else {
        while (m > 0) {
            digits[nd++] = (char)('0' + (int)(m % 10));
            m /= 10;
        }
    }
    // digits is in reverse order; reverse it.
    for (int i = 0; i < nd / 2; ++i) {
        char tmp = digits[i]; digits[i] = digits[nd-1-i]; digits[nd-1-i] = tmp;
    }
    // Build output string.
    char buf[64];
    int pos = 0;
    if (neg) buf[pos++] = '-';
    if (s == 0) {
        // Integer value.
        for (int i = 0; i < nd; ++i) buf[pos++] = digits[i];
    } else {
        // Insert decimal point s digits from the right.
        int int_digits = nd - (int)s;
        if (int_digits <= 0) {
            buf[pos++] = '0';
            buf[pos++] = '.';
            for (int i = 0; i < -(int_digits); ++i) buf[pos++] = '0';
            for (int i = 0; i < nd; ++i) buf[pos++] = digits[i];
        } else {
            for (int i = 0; i < int_digits; ++i) buf[pos++] = digits[i];
            buf[pos++] = '.';
            for (int i = int_digits; i < nd; ++i) buf[pos++] = digits[i];
        }
    }
    buf[pos] = '\0';
    // Allocate a bz_string header + data heap block (rc=1, width=1/ASCII).
    // This mirrors the layout used by bz_guid_to_string and bz_string_hdr below.
    int64_t len = (int64_t)pos;
    char* data = (char*)malloc((size_t)len + 1);
    memcpy(data, buf, (size_t)len + 1);
    // bz_string_hdr: { char* data, int64_t len, uint8_t width, int64_t rc }
    struct { char* data; int64_t len; uint8_t width; int64_t rc; }* hdr =
        (decltype(hdr))calloc(1, sizeof(*hdr));
    hdr->data  = data;
    hdr->len   = len;
    hdr->width = 1;
    hdr->rc    = 1;  // producer's reference
    return hdr;
}

// Round(digits) — rounds to `digits` decimal places using banker's rounding
// (round-half-to-even), per stdlib §9.3 ("matching Math.Round (§2.2)").
//
// Per stdlib §9.3 (v0.195, #2339): digits outside 0..28 must raise
// ArgumentOutOfRangeException. The contract is enforced by the codegen
// caller (LlvmBackend_Calls.cpp "Decimal.Round" branch emits an
// emitGuardThrow for `digits < 0 || digits > 28` BEFORE invoking this
// runtime helper), so the runtime can assume the precondition holds and
// simply compute the rounding. Defensive: if a non-codegen caller does
// invoke this helper with out-of-range digits, abort — silently clamping
// or returning the operand was the #2339 defect (Round(-1) returned -2m
// instead of raising).
extern "C" void bz_decimal_round(const bz_decimal_t* a, int32_t digits,
                                  bz_decimal_t* out) {
    if (digits < 0 || digits > 28) {
        // Precondition violation: caller should have rejected this. Write
        // default(decimal) so the out slot is left in a defined state
        // even when the abort is caught by a test harness; then bail.
        out->lo32 = 0; out->mid32 = 0; out->hi32 = 0; out->flags = 0;
        abort();  // precondition violated — codegen guards this
    }
    uint32_t s = dec_scale(a);
    if ((uint32_t)digits >= s) { *out = *a; return; }
    __uint128_t m = dec_mantissa(a);
    uint32_t remove = s - (uint32_t)digits;
    // Drop the lowest `remove` decimal places one at a time. The rounding
    // decision is made on the FIRST dropped digit (the digit at position
    // `digits`, immediately after the kept range), not on the LOWEST dropped
    // digit — that is the i == 0 iteration in this loop, which is the units
    // digit, several positions below the relevant one whenever remove > 1.
    // For example, 0.9651m.Round(2) drops two places; the relevant digit is
    // the 3rd decimal place ("5"), not the 4th ("1").  Per §9.3 banker's
    // rounding:
    //   • first-dropped < 5       → round down (no change to mantissa)
    //   • first-dropped > 5       → round up   (mantissa += 1)
    //   • first-dropped == 5 AND any later-dropped non-zero (strictly greater
    //     than the midpoint, e.g. 0.9651) → round up
    //   • first-dropped == 5 AND all later-dropped zero (exact midpoint, e.g.
    //     0.9650) → round to even: round up if the kept digit is odd, no
    //     change if it is even. 0.9650m.Round(2) keeps 96 (even) → 0.96m;
    //     0.9750m.Round(2) keeps 97 (odd)  → 0.98m.
    // Carry propagation (e.g. 0.9999m.Round(1) → 1m, 0.9950m.Round(2) → 1m
    // when the kept digit is odd) is handled by dec_store (trailing-zero
    // strip + max-96-bit reduction).
    bool dropNonZero = false;  // any later-dropped (lower-place) digit > 0
    for (uint32_t i = 0; i < remove; ++i) {
        __uint128_t rem = m % 10;
        m /= 10;
        if (i == remove - 1) {
            // First dropped digit — make the rounding decision.
            if (rem > 5) {
                m += 1;  // per §9.3
            } else if (rem < 5) {
                /* round down — no change */     // per §9.3
            } else {
                // rem == 5: banker's rounding.
                if (dropNonZero) {
                    m += 1;  // > midpoint → round up
                } else if (m % 2 != 0) {
                    m += 1;  // exact midpoint, kept digit odd → round up
                }
                // else: exact midpoint, kept digit even → round down
            }
        } else {
            // Subsequent dropped digit (lower place). Track whether the
            // dropped value exceeds an exact midpoint.
            if (rem != 0) dropNonZero = true;
        }
    }
    // Rounding only reduces mantissa magnitude; result always fits in 96 bits.
    dec_store(out, m, (uint32_t)digits, dec_sign(a), nullptr);
}

// ---------------------------------------------------------------------------
// Guid (stdlib §2.6) — 128-bit globally unique identifier.
// Representation: { uint64_t lo, uint64_t hi } (matches LLVM { i64, i64 }).
// NewGuid fills bytes from /dev/urandom and stamps RFC 4122 v4 version/variant
// bits.  ToString produces the canonical 36-char lowercase dashed form.
// Parse / TryParse accept exactly that form.
// ---------------------------------------------------------------------------

// bz_string_hdr: layout must match stringType() in LlvmBackend_impl.hpp.
// { ptr data, i64 len, i8 width, i64 rc } = { char*, int64_t, uint8_t, int64_t }.
// The rc field is at field-index 3 (offset 24 on LP64). BZ_GUID_STRING_RC_INIT is
// the "producer holds one reference" value (§4.6 runtime-managed lifetime).
static const int64_t BZ_GUID_STRING_RC_INIT = 1;

void bz_guid_new(bz_guid_t* g) {
    // Fill 16 random bytes from /dev/urandom (available on all POSIX systems
    // including Linux/Fedora 43). getrandom(2) would be slightly better but
    // would require a conditional #include inside a function which is not
    // permitted; /dev/urandom is always suitable for GUID generation.
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) { (void)fread(g, 1, 16, f); fclose(f); }
    // Stamp RFC 4122 v4 version/variant bits.
    // Byte 6 (7th byte from start): version nibble = 0100 (4).
    // Byte 8 (9th byte from start): variant bits = 10xx.
    uint8_t* b = (uint8_t*)g;
    b[6] = (b[6] & 0x0F) | 0x40;   // version 4
    b[8] = (b[8] & 0x3F) | 0x80;   // variant 10xx
}

void* bz_guid_to_string(const bz_guid_t* g) {
    // Canonical form: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars).
    // The 16 bytes are laid out in the standard UUID field order.
    const uint8_t* b = (const uint8_t*)g;
    char* buf = (char*)malloc(37);  // 36 + NUL
    snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3],
        b[4], b[5],
        b[6], b[7],
        b[8], b[9],
        b[10], b[11], b[12], b[13], b[14], b[15]);
    // Build a bz_string header wrapping the buffer.
    bz_string_hdr* s = (bz_string_hdr*)calloc(1, sizeof(bz_string_hdr));
    s->data  = buf;
    s->len   = 36;
    s->width = 1;
    s->rc    = BZ_GUID_STRING_RC_INIT;  // producer's reference (§4.6)
    return s;
}

// stdlib §2.6 / §2.18 (v0.197, #1989): Guid.ToString(format) — the format
// overload. The §2.18 Guid vocabulary is exactly "D" (canonical 36-char dashed,
// = the no-format rendering) and "N" (32 lowercase hex, no hyphens); any other
// specifier raises FormatException (§2.18 strict-fault). This mints the SAME
// fresh rc=1 heap string that bz_guid_to_string / bz_guid_to_string_n produce,
// so the result is a +1 producer (recognized in isPlusOneStringProducer). We
// return nullptr for an unrecognized/malformed specifier; the codegen caller
// then emitGuardThrow("FormatException"). Case-sensitive: lowercase "d"/"n"
// fault, matching the interpolation format path (LlvmBackend_String.cpp).
// `fmt_p` is a bz_string* (the non-nullable `format: string` parameter),
// guarded defensively.
void* bz_guid_to_string_fmt(const bz_guid_t* g, const void* fmt_p) {
    const bz_string_hdr* fh = (const bz_string_hdr*)fmt_p;
    if (fh && fh->data && fh->width == 1 && fh->len == 1) {
        char c = fh->data[0];
        if (c == 'D') return bz_guid_to_string(g);
        if (c == 'N') return bz_guid_to_string_n(g);
    }
    return nullptr;  // unrecognized specifier → FormatException (§2.18)
}

// Helper: decode two lowercase hex chars at pos, return the byte value
// or -1 on non-hex input.
static int guid_parse_hex_byte(const char* s) {
    auto hd = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = hd(s[0]); int lo = hd(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

// stdlib §2.6 (v0.195): Guid.Parse "accepts the D and N renderings (§2.18);
// FormatException otherwise". Both must parse (round-trip with the two
// ToString forms):
//   "D" — "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (exactly 36 chars, dashes at
//         positions 8, 13, 18, 23).
//   "N" — 32 hex chars, no hyphens.
// Returns 1 on success, 0 on format error. Before v0.197 (#1989) only the
// 36-char "D" form was accepted; a 32-char "N" string (as produced by
// ToString("N")) failed here, and the codegen Parse path traps (unreachable)
// on failure — so `Guid.Parse(g.ToString("N"))` crashed instead of round-
// tripping. Accepting the "N" form closes that §2.6 conformance gap.
static int32_t guid_parse_str(const char* s, int64_t len, bz_guid_t* out) {
    uint8_t bytes[16];
    if (len == 36) {
        // "D" (dashed) form.
        if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
            return 0;
        // Hex-pair start offset of each of the 16 bytes (dashes skipped).
        const int offsets[16] = {
            0, 2, 4, 6,
            9, 11,
            14, 16,
            19, 21,
            24, 26, 28, 30, 32, 34
        };
        for (int i = 0; i < 16; i++) {
            int v = guid_parse_hex_byte(s + offsets[i]);
            if (v < 0) return 0;
            bytes[i] = (uint8_t)v;
        }
        memcpy(out, bytes, 16);
        return 1;
    }
    if (len == 32) {
        // "N" (no-hyphens) form — 16 contiguous hex pairs.
        for (int i = 0; i < 16; i++) {
            int v = guid_parse_hex_byte(s + i * 2);
            if (v < 0) return 0;
            bytes[i] = (uint8_t)v;
        }
        memcpy(out, bytes, 16);
        return 1;
    }
    return 0;  // §2.6: any other length → FormatException
}

int32_t bz_guid_try_parse(const void* str, bz_guid_t* out) {
    const bz_string_hdr* s = (const bz_string_hdr*)str;
    if (!s || !s->data) return 0;
    return guid_parse_str(s->data, s->len, out);
}

int32_t bz_guid_parse(const void* str, bz_guid_t* out) {
    int32_t ok = bz_guid_try_parse(str, out);
    // On failure, zero the output (codegen traps via a separate check).
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

int32_t bz_guid_equals(const bz_guid_t* a, const bz_guid_t* b) {
    return a->lo == b->lo && a->hi == b->hi ? 1 : 0;
}

// fix #4126 / stdlib §2.6 / §8.3 (v0.295): content hash consistent with
// `bz_guid_equals` above — two Guids that compare equal (same `lo`/`hi`) MUST
// hash identically, and `Guid` has no dedicated `.GetHashCode()` interceptor
// in `tryLowerBuiltinClassCall` (`.ToString()`/`.Equals()`/`.CompareTo()` are
// the only three dedicated handlers there, LlvmBackend_Calls.cpp), so the
// call fell through to `lowerCallDispatch`'s generic universal-Object
// dispatch and degraded via #2434 (`Guid.NewGuid().GetHashCode()` measured
// zero-initialized to `0` for every distinct random Guid). Same FNV-style
// 31-multiplier fold `bz_decimal_hash` above uses, over the two 64-bit halves
// instead of a 96-bit mantissa.
extern "C" int32_t bz_guid_hash(const bz_guid_t* g) {
    uint32_t lo_lo = (uint32_t)(g->lo & 0xFFFFFFFFu);
    uint32_t lo_hi = (uint32_t)((g->lo >> 32) & 0xFFFFFFFFu);
    uint32_t hi_lo = (uint32_t)(g->hi & 0xFFFFFFFFu);
    uint32_t hi_hi = (uint32_t)((g->hi >> 32) & 0xFFFFFFFFu);
    uint32_t h = 17u;
    h = h * 31u + lo_lo;
    h = h * 31u + lo_hi;
    h = h * 31u + hi_lo;
    h = h * 31u + hi_hi;
    return (int32_t)h;
}

// per stdlib §2.6 / #1357: CompareTo — 16-byte lexicographic on canonical byte
// order (bytes 0-7 in lo field, 8-15 in hi field; stored little-endian on x86).
// Per §2.6: ordering is the same as memcmp on the 16 raw bytes.
int32_t bz_guid_compare(const bz_guid_t* a, const bz_guid_t* b) {
    int c = memcmp(a, b, 16);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// per stdlib §2.6 / #1357: FromByteArray — copy exactly 16 bytes into *out.
void bz_guid_from_bytes(const uint8_t* bytes, bz_guid_t* out) {
    memcpy(out, bytes, 16);
}
