// bezel_runtime_time.cpp — DateTime/TimeSpan/TimeOnly, numeric-to-string, Stopwatch, Debug.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// DateTime — .NET compatible tick epoch (0001-01-01 00:00:00 UTC)
// POSIX epoch is 1970-01-01 00:00:00 UTC = tick 621355968000000000
// ---------------------------------------------------------------------------

static const int64_t POSIX_EPOCH_TICKS = 621355968000000000LL;
static const int64_t TICKS_PER_SECOND  = 10000000LL;
static const int64_t TICKS_PER_MS      = 10000LL;

// per stdlib §2.5: DateTime.Now — current local DateTime as .NET ticks.
// Reads CLOCK_REALTIME (which is UTC); the local-zone offset is applied by the
// codegen wrapper / caller, not here. Intercept point per stdlib §2.5.

int64_t bz_datetime_now_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return POSIX_EPOCH_TICKS
         + (int64_t)ts.tv_sec * TICKS_PER_SECOND
         + (int64_t)ts.tv_nsec / 100;
}

// per stdlib §2.5: DateTime.UtcNow — current UTC DateTime as .NET ticks.
// CLOCK_REALTIME is already UTC, so this delegates to NowTicks. Intercept point.

int64_t bz_datetime_utcnow_ticks(void) {
    return bz_datetime_now_ticks(); // CLOCK_REALTIME is UTC
}

// per stdlib §2.5: Ticks property / FromTicks helper — decompose a tick value
// into its (year, month, day, hour, minute, second, millisecond) components.
// Underlies every DateTime scalar property accessor (Year/Month/Day/...) below.

void bz_datetime_ticks_to_parts(int64_t ticks,
                                 int32_t* year,  int32_t* month,  int32_t* day,
                                 int32_t* hour,  int32_t* minute, int32_t* second,
                                 int32_t* ms) {
    int64_t posix_sec = (ticks - POSIX_EPOCH_TICKS) / TICKS_PER_SECOND;
    int64_t rem_ticks = (ticks - POSIX_EPOCH_TICKS) % TICKS_PER_SECOND;
    time_t t = (time_t)posix_sec;
    struct tm* tm_info = gmtime(&t);
    if (year)   *year   = tm_info->tm_year + 1900;
    if (month)  *month  = tm_info->tm_mon  + 1;
    if (day)    *day    = tm_info->tm_mday;
    if (hour)   *hour   = tm_info->tm_hour;
    if (minute) *minute = tm_info->tm_min;
    if (second) *second = tm_info->tm_sec;
    if (ms)     *ms     = (int32_t)(rem_ticks / TICKS_PER_MS);
}

// per stdlib §2.5: DateTime(year, month, day, ...) / FromTicks — compose a
// tick value from components. The constructor's ArgumentOutOfRangeException
// obligation (per stdlib §2.5 / #648) is enforced by the codegen caller, not
// here; this helper is a pure inverse of ticks_to_parts.

int64_t bz_datetime_parts_to_ticks(int32_t year, int32_t month,  int32_t day,
                                    int32_t hour, int32_t minute, int32_t second,
                                    int32_t ms) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;
    time_t posix = timegm(&t);
    return POSIX_EPOCH_TICKS
         + (int64_t)posix * TICKS_PER_SECOND
         + (int64_t)ms * TICKS_PER_MS;
}

// Per §2.5: scalar property extractors from a DateTime tick value.
// All delegate through bz_datetime_ticks_to_parts for calendar accuracy.

int32_t bz_datetime_year(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return y;
}
int32_t bz_datetime_month(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return mo;
}
int32_t bz_datetime_day(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return d;
}
int32_t bz_datetime_hour(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return h;
}
int32_t bz_datetime_minute(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return mi;
}
int32_t bz_datetime_second(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return s;
}
int32_t bz_datetime_millisecond(int64_t ticks) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    return ms;
}

// DayOfWeek: 0=Sunday...6=Saturday — per §2.5 DayOfWeek enum ordering.
// Uses gmtime's tm_wday (0=Sunday, which matches the Bezel enum).
int32_t bz_datetime_day_of_week(int64_t ticks) {
    int64_t posix_sec = (ticks - POSIX_EPOCH_TICKS) / TICKS_PER_SECOND;
    time_t t = (time_t)posix_sec;
    struct tm* ti = gmtime(&t);
    return (int32_t)ti->tm_wday;  // 0=Sunday ... 6=Saturday
}

// DayOfYear: 1..366 — per §2.5; uses gmtime's tm_yday (0-based) + 1.
int32_t bz_datetime_day_of_year(int64_t ticks) {
    int64_t posix_sec = (ticks - POSIX_EPOCH_TICKS) / TICKS_PER_SECOND;
    time_t t = (time_t)posix_sec;
    struct tm* ti = gmtime(&t);
    return (int32_t)(ti->tm_yday + 1);
}

// Per §2.5: AddYears/AddMonths perform calendar-aware arithmetic.
// We decompose to components, adjust, then recompose.
int64_t bz_datetime_add_years(int64_t ticks, int32_t years) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    y += years;
    // Clamp Feb 29 in a non-leap year to Feb 28.
    if (mo == 2 && d == 29) {
        bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
        if (!leap) d = 28;
    }
    return bz_datetime_parts_to_ticks(y, mo, d, h, mi, s, ms);
}

int64_t bz_datetime_add_months(int64_t ticks, int32_t months) {
    int32_t y, mo, d, h, mi, s, ms;
    bz_datetime_ticks_to_parts(ticks, &y, &mo, &d, &h, &mi, &s, &ms);
    // Convert month to 0-based for arithmetic, then back.
    int32_t total_months = (y * 12 + (mo - 1)) + months;
    y  = total_months / 12;
    mo = total_months % 12 + 1;
    if (mo <= 0) { mo += 12; y--; }
    // Clamp day to the last day of the resulting month.
    static const int32_t days_in_month[] =
        {0, 31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
    int32_t dim = (mo == 2 && leap) ? 29 : days_in_month[mo];
    if (d > dim) d = dim;
    return bz_datetime_parts_to_ticks(y, mo, d, h, mi, s, ms);
}

// per stdlib §2.5 / #1357: ToUnixTimeSeconds — convert .NET ticks to POSIX
// seconds (truncating sub-second precision).
int64_t bz_datetime_to_unix_seconds(int64_t ticks) {
    return (ticks - POSIX_EPOCH_TICKS) / TICKS_PER_SECOND;
}

// per stdlib §2.5 / #1357: FromUnixTimeSeconds — convert POSIX seconds to
// .NET tick count (no sub-second precision).
int64_t bz_datetime_from_unix_seconds(int64_t seconds) {
    return POSIX_EPOCH_TICKS + seconds * TICKS_PER_SECOND;
}

// ---------------------------------------------------------------------------
// Per §2.5: DateTime.Parse / TryParse — ISO-8601-subset string parsing.
// Accepted formats (per §2.5; Parse throws FormatException on invalid input,
// modelled here as returning 0 with ok=0 for TryParse):
//   "yyyy-MM-dd"                         date only
//   "yyyy-MM-ddTHH:mm:ss"               date + time (T separator)
//   "yyyy-MM-dd HH:mm:ss"               date + time (space separator)
//   "yyyy-MM-ddTHH:mm:ss.fff"           with milliseconds
//   "yyyy-MM-dd HH:mm:ss.fff"           with milliseconds
// The string argument is a bz_string header (§4.6 adaptive-width).
// Returns 1 on success (result ticks written to *out), 0 on format error.
// ---------------------------------------------------------------------------

// Internal: parse an integer of exactly `width` decimal digits from s[pos..].
// Returns -1 on failure (not enough chars, non-digit). pos is advanced by width.
static int32_t parse_int_fixed(const char* s, int64_t slen,
                                int64_t* pos, int width) {
    int64_t p = *pos;
    if (p + width > slen) return -1;
    int32_t val = 0;
    for (int i = 0; i < width; ++i) {
        char c = s[p + i];
        if (c < '0' || c > '9') return -1;
        val = val * 10 + (c - '0');
    }
    *pos = p + width;
    return val;
}

// per §2.5 / #648: TryParse(s, out result) — parse ISO-8601-subset, set
// result ticks and return 1 on success, return 0 on failure (result = 0).
extern "C" int32_t bz_datetime_try_parse(const void* str_p, int64_t* out_ticks) {
    if (out_ticks) *out_ticks = 0;
    if (!str_p) return 0;
    const bz_string_hdr* h = (const bz_string_hdr*)str_p;
    if (!h->data) return 0;
    // Only width-1 (ASCII) strings are valid for date literals.
    // Per §4.6: width field at offset 16.
    if (h->width != 1) return 0;
    const char* s = h->data;
    int64_t len = h->len;
    int64_t pos = 0;

    // Parse year (4 digits), '-', month (2), '-', day (2).
    int32_t yr = parse_int_fixed(s, len, &pos, 4);
    if (yr < 0) return 0;
    if (pos >= len || s[pos] != '-') return 0; pos++;
    int32_t mo = parse_int_fixed(s, len, &pos, 2);
    if (mo < 1 || mo > 12) return 0;
    if (pos >= len || s[pos] != '-') return 0; pos++;
    int32_t dy = parse_int_fixed(s, len, &pos, 2);
    if (dy < 1) return 0;
    // per stdlib §2.5 / #2230: reject impossible calendar dates instead of
    // allowing timegm to normalize them into a different month (for example,
    // 2023-02-29 -> 2023-03-01). TryParse must return false on invalid input.
    static const int32_t days_in_month[] =
        {0, 31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (yr % 4 == 0) && (yr % 100 != 0 || yr % 400 == 0);
    int32_t max_day = (mo == 2 && leap) ? 29 : days_in_month[mo];
    if (dy > max_day) return 0;

    int32_t hr = 0, mn = 0, sc = 0, ms = 0;

    if (pos < len) {
        // Optional time component: 'T' or ' ' separator.
        char sep = s[pos];
        if (sep != 'T' && sep != ' ') return 0;
        pos++;
        hr = parse_int_fixed(s, len, &pos, 2);
        if (hr < 0 || hr > 23) return 0;
        if (pos >= len || s[pos] != ':') return 0; pos++;
        mn = parse_int_fixed(s, len, &pos, 2);
        if (mn < 0 || mn > 59) return 0;
        if (pos >= len || s[pos] != ':') return 0; pos++;
        sc = parse_int_fixed(s, len, &pos, 2);
        if (sc < 0 || sc > 59) return 0;
        // Optional fractional seconds '.fff' (exactly 3 digits accepted).
        if (pos < len && s[pos] == '.') {
            pos++;
            ms = parse_int_fixed(s, len, &pos, 3);
            if (ms < 0) return 0;
        }
    }

    // Must be at end of string after all optional components.
    if (pos != len) return 0;

    if (out_ticks)
        *out_ticks = bz_datetime_parts_to_ticks(yr, mo, dy, hr, mn, sc, ms);
    return 1;
}

// per §2.5 / #648: Parse(s: string): DateTime — on failure raises
// FormatException (not yet implemented; trap via unreachable guard in codegen).
// Returns the tick value; codegen checks the ok flag and emits an unreachable
// on failure (same pattern as Guid.Parse). For TryParse, codegen uses
// bz_datetime_try_parse directly.
extern "C" int64_t bz_datetime_parse(const void* str_p) {
    int64_t ticks = 0;
    bz_datetime_try_parse(str_p, &ticks);
    return ticks;
}

// Internal: append a zero-padded decimal integer to buf[*pos], advancing *pos.
// buf must have at least `width` bytes available.
static void fmt_int(char* buf, int* pos, int32_t val, int width) {
    char tmp[12];
    int n = snprintf(tmp, sizeof(tmp), "%d", val);
    // Left-pad with zeros to width.
    int pad = width - n;
    for (int i = 0; i < pad; ++i) buf[(*pos)++] = '0';
    for (int i = 0; i < n; ++i) buf[(*pos)++] = tmp[i];
}

// per §2.5 / §2.18 (v0.230) / #648: ToString(format: string): string — format
// a DateTime tick value using a temporal pattern. Per stdlib §2.18's
// "Temporal patterns: non-specifier characters are literals" paragraph, the
// ONLY well-formed specifier runs in the Bezel vocabulary are:
//   yyyy  → 4-digit year
//   MM    → 2-digit month (01-12)
//   dd    → 2-digit day (01-31)
//   HH    → 2-digit hour 24h (00-23)
//   mm    → 2-digit minute (00-59)
//   ss    → 2-digit second (00-59)
//   fff   → 3-digit millisecond (000-999)
// Every other character (including the specifier letters with a different run
// length — `y`, `yy`, `yyy`, `yyyyy`, `M`, `MMM`, `MMMM`, `d`, `H`, `h`, `m`,
// `s`, `f`, `ff`, `ffff`, …) is a LITERAL if it is not a specifier letter, and
// a MALFORMED SPECIFIER RUN if it is one of `y M d H m s f` — the run length
// must match exactly. A malformed run is a strict-fault (return nullptr →
// FormatException via caller; §2.18 strict-fault, §18.5 hole parsing). This
// is the v0.230 catch-up for #3292.
// Returns a fresh heap-allocated bz_string (rc=1), or nullptr on a malformed
// specifier run.
extern "C" void* bz_datetime_to_string_fmt(int64_t ticks, const void* fmt_p) {
    int32_t yr, mo, dy, hr, mn, sc, ms;
    bz_datetime_ticks_to_parts(ticks, &yr, &mo, &dy, &hr, &mn, &sc, &ms);

    const bz_string_hdr* fh = (const bz_string_hdr*)fmt_p;
    const char* fmt = (fh && fh->data && fh->width == 1) ? fh->data : "";
    int64_t flen = fh ? fh->len : (int64_t)strlen(fmt);

    // Max output: every char can expand to at most 4 digits (yyyy) + separators.
    int outCap = (int)(flen * 4 + 4);
    if (outCap < 32) outCap = 32;
    char* out = (char*)malloc((size_t)outCap + 1);
    int pos = 0;

    for (int64_t i = 0; i < flen; ) {
        char ch = fmt[i];
        // Expected run length per specifier letter; -1 means "not a specifier".
        int expected = -1;
        switch (ch) {
            case 'y': expected = 4; break;       // yyyy
            case 'M': expected = 2; break;       // MM
            case 'd': expected = 2; break;       // dd
            case 'H': expected = 2; break;       // HH
            case 'm': expected = 2; break;       // mm
            case 's': expected = 2; break;       // ss
            case 'f': expected = 3; break;       // fff
            default: break;                      // any other char: literal
        }
        if (expected < 0) {
            // Verbatim character (any non-specifier char, including 'h' — so
            // "hh" remains a literal while "HH" is the hour; per §2.18 v0.230).
            out[pos++] = ch;
            ++i;
            continue;
        }
        // Specifier letter: count the run of identical letters.
        int64_t j = i;
        while (j < flen && fmt[j] == ch) ++j;
        int64_t runLen = j - i;
        if (runLen != (int64_t)expected) {
            // Malformed specifier run (per §2.18 v0.230, #3292): e.g. "yyy",
            // "MMM", "s". Signal the fault by returning nullptr; the caller
            // raises FormatException (§2.18 strict-fault, §18.5 hole parsing).
            free(out);
            return nullptr;
        }
        switch (ch) {
            case 'y': fmt_int(out, &pos, yr, 4); break;
            case 'M': fmt_int(out, &pos, mo, 2); break;
            case 'd': fmt_int(out, &pos, dy, 2); break;
            case 'H': fmt_int(out, &pos, hr, 2); break;
            case 'm': fmt_int(out, &pos, mn, 2); break;
            case 's': fmt_int(out, &pos, sc, 2); break;
            case 'f': fmt_int(out, &pos, ms, 3); break;
            default: break;  // unreachable
        }
        i = j;
    }

    // fix #6806: NUL-terminate. The aligned-hole path (`$"{dt,21}"`) hands
    // `data` to a `%s` snprintf, which read past the unterminated buffer.
    out[pos] = '\0';
    // Build bz_string header (§4.6 layout: data/len/width/rc at offsets 0/8/16/24).
    bz_string_hdr* sh = (bz_string_hdr*)calloc(1, sizeof(bz_string_hdr));
    sh->data  = out;
    sh->len   = (int64_t)pos;
    sh->width = 1;
    sh->rc    = 1;  // producer's reference (§4.6)
    return sh;
}

// §2.18 / #1412 (v0.172+): Decimal.ToString(format) — handles the full §2.18
// decimal specifier vocabulary: F/Fn, E/En, G, P/Pn.  Returns nullptr for any
// unrecognized or malformed specifier; callers check for nullptr and raise
// FormatException (§2.18 strict-fault rule).
//
// Specifiers (per stdlib §2.18):
//   F/Fn — fixed-point, n decimal places, default 2
//   E/En — scientific notation, n places in significand, default 6;
//           3-digit exponent; sign always printed ("exact decimal semantics")
//   G    — general: exact natural representation (= plain ToString())
//   P/Pn — percent: ×100, fixed-point with n places, default 2, "%" suffix
//   anything else → nullptr (→ FormatException via caller)
//
// `fmt_p` is a bz_string {data,len,width,rc}.
extern "C" void* bz_decimal_to_string_fmt(const bz_decimal_t* a,
                                          const void* fmt_p) {
    const bz_string_hdr* fh = (const bz_string_hdr*)fmt_p;
    const char* fmt = (fh && fh->data && fh->width == 1) ? fh->data : "";
    int64_t flen = fh ? fh->len : (int64_t)strlen(fmt);
    if (flen < 1) return nullptr;  // empty format → unrecognized

    char letter = fmt[0];
    char upper = (letter >= 'a' && letter <= 'z') ? (char)(letter - 32) : letter;

    // Validate optional count tail (digits only after the letter).
    // G accepts no count per §2.18 (decimal has G, not Gn).
    bool hasCount = (flen > 1);
    int count = 0;
    if (upper == 'G') {
        if (hasCount) return nullptr;  // "Gn" not in §2.18 decimal vocabulary
    } else {
        if (hasCount) {
            for (int64_t i = 1; i < flen; ++i) {
                if (fmt[i] < '0' || fmt[i] > '9') return nullptr;  // bad tail
                count = count * 10 + (fmt[i] - '0');
                // per §2.18 / §9.3 v0.195: decimal scale is 0..28, so the count
                // tail of F/Fn, E/En, P/Pn must reject n > 28 (otherwise the
                // resulting string is not roundtrippable through Parse — the
                // call would silently produce a string whose fractional-digit
                // count exceeds the decimal invariant, raising OverflowException
                // when the user re-parses it). The off-by-one previously accepted
                // 29.
                if (count > 28) return nullptr;  // out of range
            }
        }
    }

    // ── F/Fn: fixed-point (n decimal places, default 2) — per §2.18 ──────────
    if (upper == 'F') {
        int prec = hasCount ? count : 2;  // §2.18: bare "F" defaults to 2
        bz_decimal_t r;
        bz_decimal_round(a, prec, &r);
        __uint128_t m = dec_mantissa(&r);
        uint32_t s = dec_scale(&r);   // s <= prec after rounding
        int neg = dec_sign(&r);
        bool isZero = (m == 0);
        char digits[40]; int nd = 0;
        if (isZero) { digits[0] = '0'; nd = 1; }
        else { while (m > 0) { digits[nd++] = (char)('0' + (int)(m % 10)); m /= 10; } }
        for (int i = 0; i < nd / 2; ++i) {
            char t = digits[i]; digits[i] = digits[nd-1-i]; digits[nd-1-i] = t;
        }
        char buf[80]; int pos = 0;
        if (neg && !isZero) buf[pos++] = '-';   // never "-0.00"
        int int_d = nd - (int)s;
        if (int_d <= 0) buf[pos++] = '0';
        else for (int i = 0; i < int_d; ++i) buf[pos++] = digits[i];
        if (prec > 0) {
            buf[pos++] = '.';
            int written = 0;
            for (int i = 0; i < -int_d && written < prec; ++i) {
                buf[pos++] = '0'; ++written;
            }
            int fs = int_d > 0 ? int_d : 0;
            for (int i = fs; i < nd && written < prec; ++i) {
                buf[pos++] = digits[i]; ++written;
            }
            while (written < prec) { buf[pos++] = '0'; ++written; }
        }
        buf[pos] = '\0';
        return bz_make_string(buf, pos);
    }

    // ── G: general — exact natural representation — per §2.18 #1412 ──────────
    // "Exact decimal semantics": the decimal's exact value, rendered as the
    // natural shortest decimal string (identical to plain ToString()).
    if (upper == 'G') {
        return bz_decimal_to_string(a);
    }

    // ── Shared digit extraction for P and E (digit-string domain) ─────────────
    // Working in digit-string space avoids 96-bit overflow for P (×100) and
    // lets E round the significand without re-boxing into bz_decimal_t.
    __uint128_t m0 = dec_mantissa(a);
    uint32_t s0 = dec_scale(a);
    int neg = dec_sign(a);
    bool isZero = (m0 == 0);

    // Capacity 44: up to 29 mantissa digits + 2 appended zeros (P) + guard.
    char raw[44]; int nd = 0;
    if (isZero) { raw[0] = '0'; nd = 1; }
    else {
        __uint128_t t = m0;
        while (t > 0) { raw[nd++] = (char)('0' + (int)(t % 10)); t /= 10; }
        for (int i = 0; i < nd / 2; ++i) {
            char c = raw[i]; raw[i] = raw[nd-1-i]; raw[nd-1-i] = c;
        }
    }

    // ── P/Pn: percent (×100, F-format, "%" suffix) — per §2.18 #1412 ─────────
    if (upper == 'P') {
        int prec = hasCount ? count : 2;  // §2.18 default = 2 (same as floating P)
        // Special-case zero: 0 × 100 = 0; produce "0[.00...]%".
        // Without this, appending trailing zeros to simulate ×100 would turn "0"
        // (nd=1, new_s=0) into "000" (nd=3, new_s=0), printing "000.00%".
        if (isZero) {
            char zbuf[20]; int zpos = 0;
            zbuf[zpos++] = '0';
            if (prec > 0) {
                zbuf[zpos++] = '.';
                for (int i = 0; i < prec; ++i) zbuf[zpos++] = '0';
            }
            zbuf[zpos++] = '%';
            zbuf[zpos] = '\0';
            return bz_make_string(zbuf, zpos);
        }
        // ×100 in digit-string domain: shift decimal point right by 2.
        // raw[0..nd-1] with s0 fractional digits; after ×100: new_s = s0 - 2.
        // If s0 < 2: append (2 - s0) trailing '0' chars and set new_s = 0.
        int new_s = (int)s0;
        if (new_s >= 2) {
            new_s -= 2;
        } else {
            int pad = 2 - new_s;
            for (int i = 0; i < pad && nd < 42; ++i) raw[nd++] = '0';
            new_s = 0;
        }
        // Round to prec fractional digits in digit-string domain (half-up).
        // keep_unc = total retained digits (int part + prec fractional).
        // If keep_unc < 0: all significant digits are past the prec-th fractional
        // position; the first dropped digit is a leading '0' → no carry → result is 0.
        if (new_s > prec) {
            int keep_unc = nd - new_s + prec;  // may be negative
            if (keep_unc < 0) {
                nd = 0;  // all digits past cutoff; first dropped = leading '0' → no carry
            } else if (keep_unc < nd) {
                int carry = (raw[keep_unc] - '0') >= 5 ? 1 : 0;
                nd = keep_unc;
                for (int i = nd - 1; i >= 0 && carry; --i) {
                    int dv = (raw[i] - '0') + carry;
                    raw[i] = (char)('0' + dv % 10);
                    carry = dv / 10;
                }
                if (carry) {
                    for (int i = nd; i > 0; --i) raw[i] = raw[i-1];
                    raw[0] = '1'; nd++;
                }
            }
            new_s = prec;
        }
        char buf[100]; int pos = 0;
        int int_d = nd - new_s;
        if (neg && !isZero) buf[pos++] = '-';
        if (int_d <= 0) buf[pos++] = '0';
        else for (int i = 0; i < int_d; ++i) buf[pos++] = raw[i];
        if (prec > 0) {
            buf[pos++] = '.';
            int written = 0;
            for (int i = 0; i < -int_d && written < prec; ++i) {
                buf[pos++] = '0'; ++written;
            }
            int fs = int_d > 0 ? int_d : 0;
            for (int i = fs; i < nd && written < prec; ++i) {
                buf[pos++] = raw[i]; ++written;
            }
            while (written < prec) { buf[pos++] = '0'; ++written; }
        }
        buf[pos++] = '%';
        buf[pos] = '\0';
        return bz_make_string(buf, pos);
    }

    // ── E/En: scientific notation — per §2.18 #1412 ──────────────────────────
    // "Exact decimal semantics": 3-digit exponent, sign always printed.
    // Default precision (significand digits after decimal) = 6 (§2.18; same as
    // floating E default, consistent with the single-table vocabulary).
    if (upper == 'E') {
        int prec = hasCount ? count : 6;
        // Exponent = (integer part digit count) - 1 = (nd - s0) - 1.
        int exp = isZero ? 0 : nd - (int)s0 - 1;
        // Round significand to prec+1 significant digits (half-up).
        if (!isZero && nd > prec + 1) {
            int keep = prec + 1;
            int carry = (raw[keep] - '0') >= 5 ? 1 : 0;
            nd = keep;
            for (int i = nd - 1; i >= 0 && carry; --i) {
                int dv = (raw[i] - '0') + carry;
                raw[i] = (char)('0' + dv % 10);
                carry = dv / 10;
            }
            if (carry) {
                // Carry propagated past all digits: prepend '1', bump exponent.
                for (int i = nd; i > 0; --i) raw[i] = raw[i-1];
                raw[0] = '1'; nd++;
                exp++;
                nd = prec + 1;  // re-truncate: the carry digit is now in raw[0]
            }
        }
        // Format: [-]d[0]['.'d[1]...d[prec]]E[+/-]EEE
        // 3-digit exponent per §2.18 "exact decimal semantics".
        char buf[100]; int pos = 0;
        if (neg && !isZero) buf[pos++] = '-';
        buf[pos++] = (nd >= 1) ? raw[0] : '0';
        if (prec > 0) {
            buf[pos++] = '.';
            for (int i = 1; i <= prec; ++i)
                buf[pos++] = (i < nd) ? raw[i] : '0';
        }
        buf[pos++] = letter;  // preserve E/e case (§2.18: case of specifier)
        buf[pos++] = (exp >= 0) ? '+' : '-';
        int aexp = exp < 0 ? -exp : exp;
        buf[pos++] = (char)('0' + aexp / 100);
        buf[pos++] = (char)('0' + (aexp / 10) % 10);
        buf[pos++] = (char)('0' + aexp % 10);
        buf[pos] = '\0';
        return bz_make_string(buf, pos);
    }

    return nullptr;  // unrecognized specifier → FormatException via caller
}

// ---------------------------------------------------------------------------
// v0.172 (#812/#1339, stdlib §2.18): the floating round-trip guarantee.
// `double`/`float` no-format ToString() and the "R" specifier both produce the
// SHORTEST decimal string that reparses to the identical bit pattern. Search
// precisions 1..N with "%.*e" (always a definite digit-string + exponent,
// independent of notation), reparse with strtod/strtof, and take the first
// that is bit-identical to the input (memcmp of the raw bits — distinguishes
// -0.0 from 0.0 and never mistakes a NaN payload for an exact match). Correctness
// over speed per CLAUDE.md; no caching.
//
// fix #5079 (stdlib §2.18, v0.307): §2.18 states the SHORTEST STRING, not
// merely the fewest significant digits. The prior version searched with
// "%.*g", which inherits C's own fixed/scientific switch (use scientific iff
// exponent < -4 or exponent >= precision) — a rule that is wrong once the
// digit count is itself minimized: 200.0 needs only 1 significant digit, so
// %g's own rule (exponent 2 >= precision 1) forced scientific notation
// ("2e+02", 5 chars) even though the fixed form ("200", 3 chars) is shorter
// and round-trips identically. `bz_render_shortest` below renders BOTH the
// fixed-point and scientific forms of the found minimal digit string and
// returns whichever is not longer, ties favoring fixed — issue #5079's own
// recommendation ("prefer plain decimal notation over scientific notation
// when it round-trips and is not longer"), and the only reading of "shortest
// string" that needs no invented exponent threshold.
// ---------------------------------------------------------------------------

// Build a fresh heap bz_string* (rc := 1) wrapping a copy of buf[0..len).
void* bz_make_string(const char* buf, int64_t len) {
    char* data = (char*)malloc((size_t)len + 1);
    memcpy(data, buf, (size_t)len);
    data[len] = '\0';
    bz_string_hdr* sh = (bz_string_hdr*)calloc(1, sizeof(bz_string_hdr));
    sh->data = data; sh->len = len; sh->width = 1; sh->rc = 1;
    return sh;
}

// fix #5079 (stdlib §2.18, v0.307): `sciBuf[0..sciLen)` is a round-tripping
// "%.*e"-style rendering ("[-]d[.ddd]e[+-]NN") of the shortest digit string
// found by the search loop below. Parse it into sign / digit string /
// exponent, render the equivalent fixed-point form, and choose between the
// two textual forms as follows:
//
//   - exp >= 0: this is where the reported bug lives. C's own "%g" switches
//     to scientific whenever exponent >= precision, which is wrong once the
//     digit count is itself minimized (200.0 needs only 1 significant digit,
//     so %g's rule, 2 >= 1, forces "2e+02" even though "200" is shorter and
//     round-trips identically). So for exp >= 0 compare literal lengths and
//     return whichever form is not longer, ties favoring fixed.
//   - exp < 0: this side was never buggy — C99's negative-exponent notation
//     switch (scientific iff exponent < -4) is independent of precision, so
//     it cannot be defeated by minimizing the digit count the way the
//     positive-exponent side can. A literal-length comparison here would
//     actually regress a case the issue itself confirms as correct: 0.0001
//     (exp -4) is "0.0001" (6 chars) vs "1e-04" (5 chars) — shorter
//     literally, but #5079 states the fixed form is the correct rendering.
//     So for exp < 0, keep applying the original exponent >= -4 threshold
//     directly rather than a length comparison.
static void* bz_render_shortest(const char* sciBuf, int sciLen) {
    bool neg = sciBuf[0] == '-';
    int i = neg ? 1 : 0;
    char digits[24]; int nd = 0;
    digits[nd++] = sciBuf[i++];
    if (sciBuf[i] == '.') {
        ++i;
        while (sciBuf[i] != 'e' && sciBuf[i] != 'E') digits[nd++] = sciBuf[i++];
    }
    ++i;  // skip 'e'/'E'
    int exp = atoi(sciBuf + i);

    // Fixed-point candidate: up to ~324 leading/trailing zeros at the extremes
    // of double's exponent range (DBL_TRUE_MIN / DBL_MAX), plus sign, point,
    // and up to 17 significant digits — 512 bytes is ample headroom.
    char fixedBuf[512]; int fp = 0;
    if (neg) fixedBuf[fp++] = '-';
    if (exp >= 0) {
        for (int k = 0; k <= exp; ++k)
            fixedBuf[fp++] = (k < nd) ? digits[k] : '0';
        if (nd > exp + 1) {
            fixedBuf[fp++] = '.';
            for (int k = exp + 1; k < nd; ++k) fixedBuf[fp++] = digits[k];
        }
        if (fp <= sciLen) return bz_make_string(fixedBuf, fp);
        return bz_make_string(sciBuf, sciLen);
    }

    // exp < 0: original %g-style threshold, not a length comparison (see
    // rationale above).
    if (exp >= -4) {
        fixedBuf[fp++] = '0';
        fixedBuf[fp++] = '.';
        for (int k = 0; k < -exp - 1; ++k) fixedBuf[fp++] = '0';
        for (int k = 0; k < nd; ++k) fixedBuf[fp++] = digits[k];
        return bz_make_string(fixedBuf, fp);
    }
    return bz_make_string(sciBuf, sciLen);
}

// per stdlib §2.18 (v0.344, #7301): the rendering of a NON-FINITE value.
//
// "The no-format `ToString()` and every floating specifier (`F`, `E`, `G` and
// `P`, with or without a digit count, and `R`) render a NaN as `NaN`, positive
// infinity as `Infinity` and negative infinity as `-Infinity` — the names of
// the `NaN`, `PositiveInfinity` and `NegativeInfinity` constants (§9.2) — with
// no digits, no exponent and no `%` suffix."
//
// So the spelling is the NAME of the §9.2 constant, not C's "nan"/"inf"/
// "-inf" (which is what "%g" prints). The rendering is by VALUE and not by
// origin: the NaN test comes first, so every NaN — whatever its sign bit and
// payload — renders "NaN".
static void* bz_render_non_finite(double v) {
    if (std::isnan(v)) return bz_make_string("NaN", 3);
    if (std::isinf(v) && v > 0) return bz_make_string("Infinity", 8);
    return bz_make_string("-Infinity", 9);
}

// per stdlib §2.18 (R format / no-format ToString()): double round-trip.
// Search 1..17 — IEEE 754 double's exact-decimal precision is 17 digits.
extern "C" void* bz_double_to_string_r(double v) {
    // NaN / the infinities have no digit-string/notation choice to make: the
    // spelling is fixed by §2.18 (v0.344, #7301) and is the SAME under every
    // floating specifier, so this one rendering serves the no-format
    // ToString(), the "R" specifier and (through the same runtime entry) the
    // F/E/G/P specifier paths.
    if (std::isnan(v) || std::isinf(v)) {
        return bz_render_non_finite(v);
    }
    char buf[64];
    int n = 0;
    for (int prec = 1; prec <= 17; ++prec) {
        n = snprintf(buf, sizeof(buf), "%.*e", prec - 1, v);
        double rt = strtod(buf, nullptr);
        if (memcmp(&rt, &v, sizeof(double)) == 0) break;  // exact round-trip
    }
    return bz_render_shortest(buf, n);
}

// per stdlib §2.18 (R format / no-format ToString()): float round-trip.
// Search 1..9 — IEEE 754 float's exact-decimal precision is 9 digits.
extern "C" void* bz_float_to_string_r(float v) {
    // per stdlib §2.18 (v0.344, #7301): the same three spellings as the
    // double's — "NaN", "Infinity", "-Infinity".
    if (std::isnan(v) || std::isinf(v)) {
        return bz_render_non_finite((double)v);
    }
    char buf[64];
    int n = 0;
    for (int prec = 1; prec <= 9; ++prec) {
        n = snprintf(buf, sizeof(buf), "%.*e", prec - 1, (double)v);
        float rt = strtof(buf, nullptr);
        if (memcmp(&rt, &v, sizeof(float)) == 0) break;   // exact round-trip
    }
    return bz_render_shortest(buf, n);
}

// v0.172 (stdlib §2.18): integer "B"/"Bn" — binary, no leading zeros beyond the
// natural width, left-zero-padded to `min_digits`. `value` is the operand's
// natural-width bit pattern zero-extended to 64 bits (so a signed operand's
// two's-complement bits print correctly, e.g. int -1 → 32 ones).
extern "C" void* bz_ulong_to_binary(uint64_t value, int32_t min_digits) {
    char tmp[64];
    int n = 0;
    if (value == 0) tmp[n++] = '0';
    else while (value) { tmp[n++] = (char)('0' + (int)(value & 1)); value >>= 1; }
    int total = (n < min_digits) ? min_digits : n;
    char out[80];
    int pad = total - n;
    for (int i = 0; i < pad; ++i) out[i] = '0';
    for (int i = 0; i < n; ++i) out[pad + i] = tmp[n - 1 - i];  // reverse
    return bz_make_string(out, total);
}

// v0.172 (stdlib §2.18): Guid "N" — 32 hex digits, lowercase, no hyphens.
// ("D" is the canonical bz_guid_to_string form.)
extern "C" void* bz_guid_to_string_n(const bz_guid_t* g) {
    const uint8_t* b = (const uint8_t*)g;
    char buf[33];
    for (int i = 0; i < 16; ++i)
        snprintf(buf + i * 2, 3, "%02x", b[i]);
    return bz_make_string(buf, 32);
}

// ---------------------------------------------------------------------------
// Per §2.5: TimeSpan component extractors.
// Ticks are signed 100-ns units.
// ---------------------------------------------------------------------------

static const int64_t TICKS_PER_MINUTE = 600000000LL;     // 60 * 10_000_000
static const int64_t TICKS_PER_HOUR   = 36000000000LL;   // 3600 * 10_000_000
static const int64_t TICKS_PER_DAY    = 864000000000LL;  // 86400 * 10_000_000

// v0.172 (stdlib §2.18): TimeSpan "c" (constant) format —
// `[-][d.]hh:mm:ss[.fffffff]`. The day part appears only when non-zero; the
// fractional part is 7 zero-padded digits (100-ns units) and appears only when
// non-zero. Used for the "c" specifier and the no-format rendering.
extern "C" void* bz_timespan_to_string_c(int64_t ticks) {
    char buf[48];
    int pos = 0;
    // Magnitude in uint64_t: negating long.MinValue in int64_t is signed
    // overflow (#6720); the unsigned negate is well defined.
    uint64_t u = (uint64_t)ticks;
    if (ticks < 0) { buf[pos++] = '-'; u = 0 - u; }
    int64_t days = (int64_t)(u / (uint64_t)TICKS_PER_DAY); u %= (uint64_t)TICKS_PER_DAY;
    int64_t t = (int64_t)u;
    int64_t hh   = t / TICKS_PER_HOUR; t %= TICKS_PER_HOUR;
    int64_t mm   = t / TICKS_PER_MINUTE; t %= TICKS_PER_MINUTE;
    int64_t ss   = t / TICKS_PER_SECOND; t %= TICKS_PER_SECOND;
    int64_t frac = t;  // 0..9,999,999 (100-ns units)
    if (days > 0)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%lld.", (long long)days);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02lld:%02lld:%02lld",
                    (long long)hh, (long long)mm, (long long)ss);
    if (frac > 0)
        pos += snprintf(buf + pos, sizeof(buf) - pos, ".%07lld", (long long)frac);
    return bz_make_string(buf, pos);
}

// stdlib §2.5 / §2.18 (v0.340, #6720): TimeSpan.ToString(format). `c` is the
// only specifier; anything else returns nullptr (caller raises FormatException).
extern "C" void* bz_timespan_to_string_fmt(int64_t ticks, const void* fmt_p) {
    const bz_string_hdr* fh = (const bz_string_hdr*)fmt_p;
    if (!fh || !fh->data || fh->width != 1 || fh->len != 1 || fh->data[0] != 'c')
        return nullptr;
    return bz_timespan_to_string_c(ticks);
}

void bz_timespan_components(int64_t ticks,
                             int32_t* days, int32_t* hours,
                             int32_t* minutes, int32_t* seconds,
                             int32_t* ms) {
    // Preserve sign: work with absolute value, then negate if negative.
    int32_t sign = (ticks < 0) ? -1 : 1;
    // uint64_t magnitude: -long.MinValue is signed overflow (#6720).
    int64_t t = (ticks < 0) ? (int64_t)(0 - (uint64_t)ticks) : ticks;
    int64_t d   = t / TICKS_PER_DAY;
    t %= TICKS_PER_DAY;
    int64_t h   = t / TICKS_PER_HOUR;
    t %= TICKS_PER_HOUR;
    int64_t min = t / TICKS_PER_MINUTE;
    t %= TICKS_PER_MINUTE;
    int64_t sec = t / TICKS_PER_SECOND;
    t %= TICKS_PER_SECOND;
    int64_t mss = t / TICKS_PER_MS;
    if (days)    *days    = (int32_t)(sign * d);
    if (hours)   *hours   = (int32_t)(sign * h);
    if (minutes) *minutes = (int32_t)(sign * min);
    if (seconds) *seconds = (int32_t)(sign * sec);
    if (ms)      *ms      = (int32_t)(sign * mss);
}

double bz_timespan_total_days(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_DAY;
}
double bz_timespan_total_hours(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_HOUR;
}
double bz_timespan_total_minutes(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_MINUTE;
}
double bz_timespan_total_seconds(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_SECOND;
}
double bz_timespan_total_milliseconds(int64_t ticks) {
    return (double)ticks / (double)TICKS_PER_MS;
}

// ---------------------------------------------------------------------------
// per stdlib §2.5 / #1357: TimeOnly — value type stored as i32 ms from
// midnight (0..86399999). Helpers for string conversion and parsing.
// ---------------------------------------------------------------------------

// Format ms_from_midnight as "HH:mm:ss" → fresh heap bz_string* (rc=1).
void* bz_timeonly_to_string(int32_t ms_from_midnight) {
    if (ms_from_midnight < 0) ms_from_midnight = 0;
    int32_t total_sec = ms_from_midnight / 1000;
    int32_t hh  = total_sec / 3600;
    int32_t mm  = (total_sec % 3600) / 60;
    int32_t ss  = total_sec % 60;
    // "HH:mm:ss\0" — 9 chars.
    char* out = (char*)malloc(9);
    out[0] = '0' + hh / 10;
    out[1] = '0' + hh % 10;
    out[2] = ':';
    out[3] = '0' + mm / 10;
    out[4] = '0' + mm % 10;
    out[5] = ':';
    out[6] = '0' + ss / 10;
    out[7] = '0' + ss % 10;
    out[8] = '\0';
    bz_string_hdr* sh = (bz_string_hdr*)calloc(1, sizeof(bz_string_hdr));
    sh->data  = out;
    sh->len   = 8;
    sh->width = 1;
    sh->rc    = 1;
    return sh;
}

// Parse "HH:mm:ss" or "HH:mm:ss.fff" from a bz_string*.
// Returns 1 on success (writes *out_ms in [0..86399999]), 0 on failure.
int32_t bz_timeonly_try_parse(const void* str_p, int32_t* out_ms) {
    if (out_ms) *out_ms = 0;
    if (!str_p) return 0;
    const bz_string_hdr* h = (const bz_string_hdr*)str_p;
    if (!h->data || h->width != 1) return 0;
    const char* s = h->data;
    int64_t len = h->len;
    if (len < 8) return 0;  // "HH:mm:ss" minimum
    // Parse HH
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return 0;
    int32_t hh = (s[0]-'0')*10 + (s[1]-'0');
    if (s[2] != ':') return 0;
    // Parse mm
    if (s[3] < '0' || s[3] > '9' || s[4] < '0' || s[4] > '9') return 0;
    int32_t mm = (s[3]-'0')*10 + (s[4]-'0');
    if (s[5] != ':') return 0;
    // Parse ss
    if (s[6] < '0' || s[6] > '9' || s[7] < '0' || s[7] > '9') return 0;
    int32_t ss = (s[6]-'0')*10 + (s[7]-'0');
    // Optional .fff
    int32_t ms3 = 0;
    if (len >= 12 && s[8] == '.') {
        if (s[9] < '0' || s[9] > '9' || s[10] < '0' || s[10] > '9' ||
            s[11] < '0' || s[11] > '9') return 0;
        ms3 = (s[9]-'0')*100 + (s[10]-'0')*10 + (s[11]-'0');
        if (len != 12) return 0;  // trailing chars not allowed
    } else if (len != 8) {
        return 0;  // trailing chars without .fff not allowed
    }
    // Validate ranges.
    if (hh > 23 || mm > 59 || ss > 59) return 0;
    if (out_ms) *out_ms = hh*3600000 + mm*60000 + ss*1000 + ms3;
    return 1;
}

// ---------------------------------------------------------------------------
// §8.1 System.Diagnostics.Stopwatch runtime
//
// Representation: bz_stopwatch_t* (heap pointer, owned by the Bezel Stopwatch
// instance). Layout mirrors the .NET semantics: accumulated elapsed_ns holds
// time from previous Start/Stop cycles; start_ts holds the CLOCK_MONOTONIC
// timestamp at the most recent Start() call (valid only when running == 1).
//
// ElapsedMilliseconds = (elapsed_ns + running_ns) / 1_000_000  (i64)
// Elapsed (TimeSpan)  = (elapsed_ns + running_ns) / 100        (i64, 100-ns ticks)
//
// All functions are extern "C" with the bz_sw_* prefix.
// ---------------------------------------------------------------------------

struct bz_stopwatch_t {
    struct timespec start_ts;  // CLOCK_MONOTONIC timestamp of last Start()
    int64_t elapsed_ns;        // accumulated nanoseconds from completed Start/Stop cycles
    int32_t running;           // 1 = running, 0 = stopped
};

// Internal: nanoseconds since start_ts (only call when running == 1).
static int64_t bz_sw_running_ns(const bz_stopwatch_t* sw) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec_diff  = (int64_t)now.tv_sec  - (int64_t)sw->start_ts.tv_sec;
    int64_t nsec_diff = (int64_t)now.tv_nsec - (int64_t)sw->start_ts.tv_nsec;
    return sec_diff * (int64_t)1000000000 + nsec_diff;
}

extern "C" {

// per stdlib §8.1: Stopwatch() — allocate and zero-initialise.
void* bz_stopwatch_alloc(void) {
    auto* sw = (bz_stopwatch_t*)calloc(1, sizeof(bz_stopwatch_t));
    return sw;
}

// per stdlib §8.1: Stopwatch.Dispose() — free the heap object.
void bz_stopwatch_free(void* swp) {
    free(swp);
}

// per stdlib §8.1: Stopwatch.Start() — begin (or resume) timing.
void bz_stopwatch_start(void* swp) {
    auto* sw = (bz_stopwatch_t*)swp;
    if (!sw || sw->running) return;  // idempotent: already running → no-op
    clock_gettime(CLOCK_MONOTONIC, &sw->start_ts);
    sw->running = 1;
}

// per stdlib §8.1: Stopwatch.Stop() — pause timing; accumulate elapsed.
void bz_stopwatch_stop(void* swp) {
    auto* sw = (bz_stopwatch_t*)swp;
    if (!sw || !sw->running) return;  // idempotent: already stopped → no-op
    sw->elapsed_ns += bz_sw_running_ns(sw);
    sw->running = 0;
}

// per stdlib §8.1: Stopwatch.Reset() — stop (if running) and zero elapsed.
void bz_stopwatch_reset(void* swp) {
    auto* sw = (bz_stopwatch_t*)swp;
    if (!sw) return;
    sw->running = 0;
    sw->elapsed_ns = 0;
    sw->start_ts.tv_sec  = 0;
    sw->start_ts.tv_nsec = 0;
}

// per stdlib §8.1: Stopwatch.IsRunning — returns 1 (true) or 0 (false).
int32_t bz_stopwatch_is_running(void* swp) {
    auto* sw = (bz_stopwatch_t*)swp;
    if (!sw) return 0;
    return sw->running;
}

// per stdlib §8.1: Stopwatch.ElapsedMilliseconds — total elapsed as i64 ms.
int64_t bz_stopwatch_elapsed_ms(void* swp) {
    auto* sw = (bz_stopwatch_t*)swp;
    if (!sw) return 0;
    int64_t ns = sw->elapsed_ns;
    if (sw->running) ns += bz_sw_running_ns(sw);
    return ns / (int64_t)1000000;
}

// per stdlib §8.1: Stopwatch.Elapsed (TimeSpan) — total elapsed as i64 ticks.
int64_t bz_stopwatch_elapsed_ticks(void* swp) {
    auto* sw = (bz_stopwatch_t*)swp;
    if (!sw) return 0;
    int64_t ns = sw->elapsed_ns;
    if (sw->running) ns += bz_sw_running_ns(sw);
    return ns / (int64_t)100;   // nanoseconds → 100-ns ticks
}

} // extern "C" (Stopwatch)

// ---------------------------------------------------------------------------
// §8.2 System.Diagnostics.Debug runtime
//
// Debug.Assert(condition) / Debug.Assert(condition, message):
//   If condition is false, write the assertion message to stderr and call
//   abort(). This implementation is always-on (no --debug flag yet, per
//   recorded choice in diagnostics_646_test.cpp §8.2 gap note 2).
//
// Debug.WriteLine(message):
//   Write message followed by a newline to stderr. Always-on.
// ---------------------------------------------------------------------------

extern "C" {

// bz_debug_assert_fail(msg_p) — print message to stderr and abort.
// msg_p is a bz_string* (the string object; we extract data/len the same way
// Console.Write does). Called only when the condition is false.
void bz_debug_assert_fail(void* msg_p) {
    if (msg_p) {
        // bz_string layout: data ptr @ offset 0, len (code-point count) @ offset 8,
        // width byte @ offset 16. UTF-8 (width=1) path only; mirror Console.Write.
        const uint8_t* hdr = (const uint8_t*)msg_p;
        void* data_ptr;
        memcpy(&data_ptr, hdr, sizeof(void*));
        int64_t len;
        memcpy(&len, hdr + 8, sizeof(int64_t));
        uint8_t width = hdr[16];
        // Only width-1 (UTF-8) is printable without transcoding; for wider
        // encodings fall through to the generic abort message.
        if (width == 1 && data_ptr && len > 0) {
            fputs("Assertion failed: ", stderr);
            fwrite(data_ptr, 1, (size_t)len, stderr);
            fputc('\n', stderr);
            fflush(stderr);
            abort();
        }
    }
    fputs("Assertion failed\n", stderr);
    fflush(stderr);
    abort();
}

// bz_debug_writeline(msg_p) — write message + newline to stderr.
void bz_debug_writeline(void* msg_p) {
    if (!msg_p) { fputs("\n", stderr); return; }
    const uint8_t* hdr = (const uint8_t*)msg_p;
    void* data_ptr;
    memcpy(&data_ptr, hdr, sizeof(void*));
    int64_t len;
    memcpy(&len, hdr + 8, sizeof(int64_t));
    uint8_t width = hdr[16];
    if (width == 1 && data_ptr && len > 0) {
        fwrite(data_ptr, 1, (size_t)len, stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

} // extern "C" (Debug)
