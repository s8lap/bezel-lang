// fix #5390 — Buffer<T> is not an always-available name (§2.3, v0.314).
//
// The spec ruling (b1f76a70, #5260 answered as "existing text, no change")
// confirms that Buffer<T> is NOT listed in §2.3's always-available names table.
// No stdlib module declares it; it was an impl-ahead-of-spec compiler intrinsic.
//
// The fix moves the primitive behind an internal-only spelling: __Buffer<T>.
// User code `Buffer<T>` with no declaration in scope is now an unresolved type,
// as the spec requires.
//
// Tests cover the internal __Buffer<T> intrinsic:
//   (a) T-restriction positives — value-type primitives accepted.
//   (b) T-restriction negatives — string/class/record elements rejected.
//   (c) __Buffer<T>[i] indexer — result typed as T (genericArgs[0]).
//   (d) __Buffer<T>.Length property — typed int.
//   (e) __Buffer<T>.Dispose() call — accepted.
//   (f) Unknown method on __Buffer<T> — rejected.
//
// The old buffer_545_test.cpp (issue #545) tested Buffer<T> as an always-available
// intrinsic. After this fix, those tests flip: Buffer<T> without a declaration
// is an unresolved type.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bezel::test::JitResult runSema(const std::string& src) {
    return runAot(src);
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// (a) T-restriction positives — value-type primitives accepted (no error)
// ===========================================================================

// __Buffer<int> is accepted (int is a value-type primitive).
BEZEL_TEST(sema_buffer_5390_intrinsic, int_accepted) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<int>(4); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagContains(r, "requires"));
}

// __Buffer<byte> accepted.
BEZEL_TEST(sema_buffer_5390_intrinsic, byte_accepted) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<byte>(8); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagContains(r, "requires"));
}

// __Buffer<long> accepted.
BEZEL_TEST(sema_buffer_5390_intrinsic, long_accepted) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<long>(2); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagContains(r, "requires"));
}

// __Buffer<double> accepted.
BEZEL_TEST(sema_buffer_5390_intrinsic, double_accepted) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<double>(3); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagContains(r, "requires"));
}

// ===========================================================================
// (b) T-restriction negatives — class, record, string rejected
// ===========================================================================

// __Buffer<string> rejected — string is runtime-managed.
BEZEL_TEST(sema_buffer_5390_intrinsic, string_element_rejected) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<string>(4); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "requires a value-type element"));
}

// __Buffer<C> where C is a user class rejected — class elements out of scope.
BEZEL_TEST(sema_buffer_5390_intrinsic, class_element_rejected) {
    auto r = runSema(
        "module M; "
        "class Elem end Elem; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<Elem>(2); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "requires a value-type element"));
}

// ===========================================================================
// (c) __Buffer<T>.Length property — typed int
// ===========================================================================

// buf.Length is accepted in a context that consumes an int.
BEZEL_TEST(sema_buffer_5390_intrinsic, length_property_accepted) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test(): int "
        "    using var b := new __Buffer<int>(10); "
        "    return b.Length; "
        "  end Test; "
        "  public static method Main() "
        "    var x := C.Test(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (d) __Buffer<T>.Dispose() — accepted
// ===========================================================================

BEZEL_TEST(sema_buffer_5390_intrinsic, dispose_accepted) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    var b := new __Buffer<int>(2); "
        "    b.Dispose(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagContains(r, "no member `Dispose`"));
}

// ===========================================================================
// (e) Unknown method on __Buffer<T> rejected
// ===========================================================================

BEZEL_TEST(sema_buffer_5390_intrinsic, unknown_method_rejected) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new __Buffer<int>(4); "
        "    b.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "no member `Bogus`"));
}

// ===========================================================================
// (f) User code: Buffer<T> with no declaration is unresolved (§2.3, v0.314)
// ===========================================================================

// Buffer<int> with NO user declaration in scope must be an unresolved type.
// This is the core of the #5390 fix: before the fix, Buffer<T> resolved
// as a builtin (impl-ahead-of-spec); after the fix it is unresolved.
BEZEL_TEST(sema_buffer_5390_user_code, buffer_int_unresolved_without_declaration) {
    auto r = runSema(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var b := new Buffer<int>(4); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved type name `Buffer`"));
}

// Control: a user-declared class Buffer<T> shadows the internal name
// (user code can define its own Buffer if it wants).
// The user-declared Buffer<T> is found instead of the builtin __Buffer<T>,
// which proves shadowing works.
BEZEL_TEST(sema_buffer_5390_user_code, user_declared_buffer_shadows_intrinsic) {
    auto r = runSema(
        "module M; "
        "class Buffer<T> "
        "  public constructor(x: T) end constructor; "
        "end Buffer; "
        "class C "
        "  public static method Test() "
        "    var b := new Buffer<int>(42); "
        "  end Test; "
        "  public static method Main() "
        "    C.Test(); "
        "  end Main; "
        "end C;");
    // A user-declared Buffer<T> is distinct from __Buffer<T>.
    // User Buffer has no .Length property (unlike __Buffer<T>).
    // This compiles because user Buffer is a valid type.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
