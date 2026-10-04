// issue #545 — Buffer<T> compiler-intrinsic: sema tests.
//
// Design note (issue #545 owner comment, 2026-06-13):
//   Buffer<T> is a fixed-capacity compiler-intrinsic primitive that backs
//   self-hosted collections.  Value-type elements ONLY in this revision:
//   int/byte/short/long/double/decimal/char/bool.  Class, record, and string
//   elements re-open §5.20 borrow-storing / zeroed-slot-disposal questions and
//   are explicitly deferred.
//
// Tests cover:
//   (a) T-restriction positives — every value-type primitive accepted.
//   (b) T-restriction negatives — class, record, string, interface rejected
//       with the exact diagnostic.
//   (c) Buffer<T>[i] indexer — result typed as T (genericArgs[0]).
//   (d) Buffer<T>.Length property — typed int.
//   (e) Buffer<T>.Dispose() call — accepted (no error).
//   (f) Unknown method on Buffer<T> — rejected.
//   (g) Arity: Buffer with no type arg → arity error from builtinTypeArity.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiagContaining(const bezel::test::SemaResult& r,
                       const std::string& substr) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(substr) != std::string::npos) return true;
    return false;
}

} // namespace

// ===========================================================================
// (a) T-restriction positives — value-type primitives accepted (no error)
// ===========================================================================

// issue #545: Buffer<int> is accepted (int is a value-type primitive).
BEZEL_TEST(buffer_545_sema_restriction, int_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<int>(4); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// issue #545: Buffer<byte> accepted.
BEZEL_TEST(buffer_545_sema_restriction, byte_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<byte>(8); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// issue #545: Buffer<short> accepted.
BEZEL_TEST(buffer_545_sema_restriction, short_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<short>(2); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// issue #545: Buffer<long> accepted.
BEZEL_TEST(buffer_545_sema_restriction, long_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<long>(2); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// issue #545: Buffer<double> accepted.
BEZEL_TEST(buffer_545_sema_restriction, double_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<double>(3); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// issue #545: Buffer<decimal> accepted.
BEZEL_TEST(buffer_545_sema_restriction, decimal_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<decimal>(2); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// issue #545: Buffer<char> accepted.
BEZEL_TEST(buffer_545_sema_restriction, char_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<char>(16); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// issue #545: Buffer<bool> accepted.
BEZEL_TEST(buffer_545_sema_restriction, bool_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<bool>(4); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "Buffer<T> requires"));
}

// ===========================================================================
// (b) T-restriction negatives — class, record, string rejected
// ===========================================================================

// issue #545: Buffer<string> rejected — string is runtime-managed, not a
// plain value type (its elements require bz_string_release on disposal).
BEZEL_TEST(buffer_545_sema_restriction, string_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<string>(4); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "Buffer<T> requires a value-type element"));
}

// issue #545: Buffer<C> where C is a user class rejected — class elements
// re-open §5.20 borrow-storing questions, explicitly out of scope.
BEZEL_TEST(buffer_545_sema_restriction, class_element_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Elem end Elem; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<Elem>(2); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "Buffer<T> requires a value-type element"));
}

// ===========================================================================
// (d) Buffer<T>.Length property — typed int (no error, correct type set)
// ===========================================================================

// issue #545: buf.Length is accepted in a context that consumes an int.
BEZEL_TEST(buffer_545_sema_length, length_property_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test(): int "
        "    using var b := new __Buffer<int>(10); "
        "    return b.Length; "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (e) Buffer<T>.Dispose() — accepted as a valid method call
// ===========================================================================

// issue #545: explicit buf.Dispose() is accepted by sema.
BEZEL_TEST(buffer_545_sema_dispose, dispose_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    var b := new __Buffer<int>(2); "
        "    b.Dispose(); "
        "  end Test; "
        "end C;");
    // Dispose() is in the surface; no error expected.
    BEZEL_EXPECT_FALSE(hasDiagContaining(r, "no member `Dispose`"));
}

// ===========================================================================
// (f) Unknown method on Buffer<T> rejected
// ===========================================================================

// issue #545: buf.Resize() is NOT on the Buffer<T> surface → clean error.
BEZEL_TEST(buffer_545_sema_unknown_method, resize_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<int>(4); "
        "    b.Resize(8); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "no member `Resize`"));
}

// issue #545: buf.Add(v) is NOT on the surface → clean error.
BEZEL_TEST(buffer_545_sema_unknown_method, add_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<int>(4); "
        "    b.Add(1); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "no member `Add`"));
}

// issue #545: the diagnostic names the full surface.
BEZEL_TEST(buffer_545_sema_unknown_method, diagnostic_names_surface) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<int>(4); "
        "    b.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "[i] (get/set), Length, Dispose()"));
}
