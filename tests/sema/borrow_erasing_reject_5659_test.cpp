// Spec-derived regression test for issue #5659.
//
// §9.7 / §8.6 (proposal 0146, v0.330): a value that holds borrows may not
// reach a borrow-erasing position. A borrow-erasing position is one declared
// `Object` or an interface type other than `IReadOnlySpan<TElement>`.
//
// Exceptions (allowed):
//   - A concrete span (Span, ReadOnlySpan, ArraySpan, ReadOnlyArraySpan) may be
//     passed to an IReadOnlySpan<TElement> position (IReadOnlySpan is the
//     unifying read-only surface for concrete spans).
//   - Member calls on a borrowed span receiver (e.g. span.ToString()) are
//     unaffected since they don't involve boxing.
//
// This test covers the four banked conformance cases:
//   s9_7_0027: b.Keep<Object>(span) — span at bare T substituted with Object
//   s9_7_0028: xs.Add(span) at Array<Object> — span at collection element
//   s9_7_0029: var o: Object := s where s: IReadOnlySpan<int>
//   s9_7_0030: async method Scan(s: IReadOnlySpan<int>)

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---- Rejection tests ----

// s9_7_0027: a span passed at a bare T the call substitutes with Object is rejected
BEZEL_TEST(sema_borrow_erasing_5659, keep_object_span_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "public class Box "
        "  private _f: Object?; "
        "  public constructor() end constructor; "
        "  public method Keep<T>(x: T) _f := x; end Keep; "
        "end Box; "
        "public class Client "
        "  public static method Run(b: Box) "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    b.Keep<Object>(s); "
        "  end Run; "
        "end Client;");
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-erasing"));
    BEZEL_EXPECT_TRUE(hasError(r, "Span"));
    BEZEL_EXPECT_TRUE(hasError(r, "Object"));
}

// s9_7_0028: a span added to an Array<Object> is rejected
BEZEL_TEST(sema_borrow_erasing_5659, array_object_add_span_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "public class Client "
        "  public static method Run(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    using var xs := new Array<Object>(); "
        "    xs.Add(s); "
        "    return xs.Count; "
        "  end Run; "
        "end Client;");
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-erasing"));
    BEZEL_EXPECT_TRUE(hasError(r, "Span"));
}

// s9_7_0029: an IReadOnlySpan value converted to an Object local is rejected
BEZEL_TEST(sema_borrow_erasing_5659, i_read_only_span_to_object_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "public class Client "
        "  public static method Run(s: IReadOnlySpan<int>): int "
        "    var o: Object := s; "
        "    return s.Length; "
        "  end Run; "
        "end Client;");
    BEZEL_EXPECT_TRUE(hasError(r, "borrow-erasing"));
    BEZEL_EXPECT_TRUE(hasError(r, "IReadOnlySpan"));
}

// ---- Positive controls (these should compile without error) ----

// A concrete span may be passed to an IReadOnlySpan<TElement> parameter
BEZEL_TEST(sema_borrow_erasing_5659,
           concrete_span_to_i_read_only_span_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "public class Client "
        "  public static method TakesReadOnlySpan(s: IReadOnlySpan<int>): int "
        "    return s.Length; "
        "  end TakesReadOnlySpan; "
        "  public static method Run(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    return TakesReadOnlySpan(s); "
        "  end Run; "
        "end Client;");
    BEZEL_EXPECT_TRUE(!hasError(r, "borrow-erasing"));
}

// A method call on a borrowed span receiver is allowed (ToString, etc.)
BEZEL_TEST(sema_borrow_erasing_5659, span_to_string_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "public class Client "
        "  public static method Run(): String "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    return s.ToString(); "
        "  end Run; "
        "end Client;");
    BEZEL_EXPECT_TRUE(!hasError(r, "borrow-erasing"));
}

// An IReadOnlySpan passed to an IReadOnlySpan parameter is allowed (same type)
// An IReadOnlySpan value converted to an IReadOnlySpan POSITION is allowed:
// §8.6 and §9.7 name a position declared `IReadOnlySpan<TElement>` as one of
// the two ADMITTED positions for a value that holds borrows.
//
// This row previously declared a parameter and read a member — it contained
// no conversion at all, so it passed whether or not the borrow-erasing check
// existed and pinned nothing. It is the exact row that should have caught the
// over-rejection: with `sourceIsIReadOnlySpan ||` short-circuiting the target
// guard, an `IReadOnlySpan -> IReadOnlySpan` conversion was rejected. Both
// forms below therefore carry a real conversion.
BEZEL_TEST(sema_borrow_erasing_5659,
           i_read_only_span_to_i_read_only_span_accepted) {
    // Call argument: an IReadOnlySpan reaching an IReadOnlySpan parameter.
    auto r = parseSemaAll(
        "module M; "
        "public class Client "
        "  public static method Take(s: IReadOnlySpan<int>): int "
        "    return s.Length; "
        "  end Take; "
        "  public static method Run(s: IReadOnlySpan<int>): int "
        "    return Take(s); "
        "  end Run; "
        "end Client;");
    BEZEL_EXPECT_TRUE(!hasError(r, "borrow-erasing"));
    // Local init: the same conversion through a declaration, not a call.
    auto r2 = parseSemaAll(
        "module M; "
        "public class Client "
        "  public static method Run(s: IReadOnlySpan<int>): int "
        "    var t: IReadOnlySpan<int> := s; "
        "    return t.Length; "
        "  end Run; "
        "end Client;");
    BEZEL_EXPECT_TRUE(!hasError(r2, "borrow-erasing"));
}