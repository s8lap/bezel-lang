// The `??` MEET's ownership classification — §28.3's producers table (the
// `a ?? b` row AND the collection-literal row), K7.1–K7.3, K7.5; §7.7; §15.4;
// §28.3 *Borrow lifetime rules*.
//
// TWO DEFECTS, ONE MECHANISM (issues #8371 and #8373). `a ?? b` is a MEET of
// the two arms' ownership states, and the compiler's ownership pass
// mis-classified the meet's arms in two different ways, so a `??` slipped past
// two §28.3 rules that its sibling constructs obey:
//
//   * #8371 — a COLLECTION-LITERAL arm is classified BORROWED, so `a ?? [1]`
//     reads as `borrowed ⊓ borrowed` instead of `owned ⊓ borrowed`: the MIXED
//     meet is NOT rejected (its `if`-expression and `new` twins both ARE), and
//     the literal's fresh backing is never discharged — it LEAKS (and, through
//     an `owned` return, DOUBLE-FREES).
//   * #8373 — a `??` that is really a BORROW (both arms borrows) is returned
//     through an `owned` return and is NOT rejected, while the very same
//     borrow returned directly (`return a;`) IS — so the program DOUBLE-FREES
//     at runtime.
//
// SPEC AUTHORITY.
//
// §28.3.4 *Producers*, the collection-literal row (spec:7130): "Collection
// literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
// `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned".
//
// §28.3.4 *Producers*, the `a ?? b` row (spec:7149): "The **meet** of the
// ownership states of `a` and `b` per K7 — … owned ⊓ owned = **owned**, exactly
// one arm's value produced and discharged; borrowed ⊓ borrowed = **borrowed**
// with the union of sources; mixed = **rejected** — equalize the arms first.
// … A `null`-literal or bottom-typed (`throw`) arm absorbs to the other arm's
// state (K7.5)".
//
// §28.3.8 *Borrow lifetime rules* (spec:7503): "A borrowed local may not be
// returned from a method whose return type is `owned`."
//
// §7.7 (spec:1071): "for a left operand of type `T?`, the right operand must be
// implicitly convertible (§5.1) to `T` or to `T?`" — so the MEET's own type is
// the target type a collection-literal right operand was written against.
//
// §15.4: a collection literal is CONTEXT-TYPED ("A collection literal has no
// standalone type. It is well-typed only in a context that provides the target
// type"), so sema records NO type on the literal node — which is exactly why
// the ownership pass never applied §28.3's collection-literal row to it.
//
// THE FIX. `src/ownership/Ownership_Checks.cpp`:
//   * the `??` meet's arms are classified with §28.3's collection-literal row
//     applied (the MEET's own type is the literal's §15.4 context), so the
//     mixed meet `a ?? [1]` is REJECTED with the §28.3 K7.3 diagnostic;
//   * the borrow classifier that answers "is this expression a borrow?" (the
//     one the §28.3 *Borrow lifetime rules* rule reads) now classifies the
//     `??` MEET: borrowed ⊓ borrowed is a BORROW, so `return a ?? b;` through
//     an `owned` return is REJECTED with the SAME diagnostic the direct form
//     `return a;` produces.
//
// OBSERVABILITY. Each REJECT row asserts the SPECIFIC diagnostic and
// `!r.ran` (the program never runs). The ACCEPT rows are the spec-legal
// neighbours, run under runAotChecked (generated-code ASan/LSan) with exact
// dispose counts, so a leak or a double-free in them is a failure.
//
// per §28.3 (producers table — the `a ?? b` row and the collection-literal
// row; K7.1–K7.3, K7.5), §28.3.8 *Borrow lifetime rules*, §7.7, §15.4,
// v0.355, #8371, #8373

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §28.3 K7.3 mixed-meet diagnostic (the `??` half).
const char* kMixedMeet =
    "the operands of this `??` mix an owned value with a borrowed arm";
// The §28.3 *Borrow lifetime rules* diagnostic (the owned-return half).
const char* kBorrowReturn =
    "a borrow cannot be returned as an `owned` value";
// The §28.3 owned-field store diagnostic (the owned-field-sink half).
const char* kBorrowField =
    "a borrow cannot be stored into an `owned` field";

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

void expectRejected(const std::string& src, const std::string& needle) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, needle));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A user class with a printing destructor, so the dispose count is
// observable. `IDisposable` is the §A.4 shape the destructor spelling needs.
const char* kWidgetClass =
    "public class Widget : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end Widget; ";

}  // namespace

// ===========================================================================
// #8371 — a COLLECTION-LITERAL arm of a `??` is an OWNED arm, so `a ?? [1]`
// is a MIXED meet and is rejected (§28.3 producers table; K7.3).
// ===========================================================================

// The issue's program: `return a ?? [1];` where `a` is a BORROWED parameter,
// through an `: owned Array<int>` return. On the base this compiled clean and
// double-freed at runtime.
BEZEL_TEST(coalesce_meet_8371_8373,
           collection_literal_arm_mixed_meet_rejected_8371_28_3) {
    expectRejected(
        "module M; import System; "
        "public class C "
        "  public static method Bad(a: Array<int>): owned Array<int> "
        "    return a ?? [1]; "
        "  end Bad; "
        "  public static method Main(): int return 0; end Main; "
        "end C;",
        kMixedMeet);
}

// The same meet in a NON-return position — the §28.3 K7.3 rule rejects a mixed
// meet "in EVERY position, not only at a binding or a return" (§28.3, the
// `a ?? b` row).
BEZEL_TEST(coalesce_meet_8371_8373,
           collection_literal_arm_mixed_meet_local_rejected_8371_28_3) {
    expectRejected(
        "module M; import System; "
        "public class C "
        "  public static method Bad(a: Array<int>): int "
        "    var y := a ?? [1]; "
        "    return y.Count; "
        "  end Bad; "
        "  public static method Main(): int return 0; end Main; "
        "end C;",
        kMixedMeet);
}

// The same meet inside a MEMBER-ACCESS chain — the peel that reaches the
// `??` under `(_w ?? [1]).Count`.
BEZEL_TEST(coalesce_meet_8371_8373,
           collection_literal_arm_mixed_meet_member_chain_rejected_8371_28_3) {
    expectRejected(
        "module M; import System; "
        "public class C "
        "  public static method Bad(a: Array<int>): int "
        "    return (a ?? [1]).Count; "
        "  end Bad; "
        "  public static method Main(): int return 0; end Main; "
        "end C;",
        kMixedMeet);
}

// ===========================================================================
// #8373 — a BORROWED meet returned through an `owned` return is rejected
// (§28.3 *Borrow lifetime rules*), exactly as the direct form is.
// ===========================================================================

// The issue's program: `return a ?? b;` where both arms are BORROWED
// parameters, through an `: owned Array<int>` return. On the base this
// compiled clean and double-freed at runtime (ASan: "attempting
// double-free").
BEZEL_TEST(coalesce_meet_8371_8373,
           borrowed_meet_through_owned_return_rejected_8373_28_3) {
    expectRejected(
        "module M; import System; "
        "public class C "
        "  public static method Bad(a: Array<int>, b: Array<int>): "
        "owned Array<int> "
        "    return a ?? b; "
        "  end Bad; "
        "  public static method Main(): int "
        "    var r: Array<int> := C.Bad([7, 8, 9], [1]); "
        "    return r.Count; "
        "  end Main; "
        "end C;",
        kBorrowReturn);
}

// ===========================================================================
// CONTROLS — the spec-legal neighbours and the sibling constructs. These are
// GREEN on the base as well; a change in any of them is a regression.
// ===========================================================================

// The `new` TWIN of #8371: `g ?? new W(1)` is the same mixed meet and was
// ALREADY rejected on the base — the control that proves the mixed-meet
// machinery works and that this fix did not replace it with something else.
BEZEL_TEST(coalesce_meet_8371_8373,
           new_twin_mixed_meet_still_rejected_28_3) {
    expectRejected(
        "module M; import System; "
        "public class W "
        "  public constructor(n: int) end constructor; "
        "  destructor end destructor; "
        "end W; "
        "public class C "
        "  public static method Bad(g: W?, p: W): owned W "
        "    return g ?? new W(1); "
        "  end Bad; "
        "  public static method Main(): int return 0; end Main; "
        "end C;",
        kMixedMeet);
}

// The `if`-EXPRESSION TWIN of #8371: a collection literal cannot be an `if`
// branch (§20) — already refused on the base, and the fix does not touch it.
BEZEL_TEST(coalesce_meet_8371_8373,
           if_expression_twin_literal_branch_still_rejected_20) {
    expectRejected(
        "module M; import System; "
        "public class C "
        "  public static method Bad(a: Array<int>, c: bool): owned Array<int> "
        "    return (if c then a else [1]); "
        "  end Bad; "
        "  public static method Main(): int return 0; end Main; "
        "end C;",
        "a collection literal cannot be an `if` expression branch");
}

// The DIRECT-FORM control of #8373: `return a;` through an `: owned` return —
// the same diagnostic the `??` spelling now produces.
BEZEL_TEST(coalesce_meet_8371_8373,
           direct_borrow_return_still_rejected_28_3) {
    expectRejected(
        "module M; import System; "
        "public class C "
        "  public static method Bad(a: Array<int>): owned Array<int> "
        "    return a; "
        "  end Bad; "
        "  public static method Main(): int "
        "    var r: Array<int> := C.Bad([7, 8, 9]); "
        "    return r.Count; "
        "  end Main; "
        "end C;",
        kBorrowReturn);
}

// The LEGAL `owned ⊓ owned` meet returned through an `owned` return — it must
// still compile, and the selected arm's value must be discharged EXACTLY ONCE
// (the other arm never runs, so it is never produced).
BEZEL_TEST(coalesce_meet_8371_8373,
           legal_owned_meet_through_owned_return_disposes_once_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; import System; ") + kWidgetClass +
        "public class C "
        "  public static method Make(): owned Widget return new Widget(1); "
        "  end Make; "
        "  public static method Bad(): owned Widget "
        "    return Make() ?? new Widget(2); "
        "  end Bad; "
        "  public static method Main(): int "
        "    using var r := Bad(); "
        "    Console.WriteLine(\"body\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The selected arm's value is disposed exactly once; the other arm never
    // ran, so it was never produced.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 0);
}

// The legal `owned ⊓ owned` meet in a NON-return position — the local is an
// owned value and is auto-dropped at scope end, exactly once.
BEZEL_TEST(coalesce_meet_8371_8373,
           legal_owned_meet_local_disposes_once_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; import System; ") + kWidgetClass +
        "public class C "
        "  public static method Make(): owned Widget return new Widget(1); "
        "  end Make; "
        "  public static method Bad() "
        "    var y := Make() ?? new Widget(2); "
        "    Console.WriteLine(\"body\"); "
        "  end Bad; "
        "  public static method Main(): int "
        "    Bad(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 0);
}

// The BORROWED meet in a NON-return position — a borrow is not a sink, so the
// meet stays legal. The fix rejects the borrowed meet only where it reaches an
// OWNED sink.
BEZEL_TEST(coalesce_meet_8371_8373,
           borrowed_meet_local_still_accepted_28_3) {
    JitResult r = runAot(
        "module M; import System; "
        "public class C "
        "  public static method Bad(a: Array<int>, b: Array<int>): int "
        "    var y := a ?? b; "
        "    return y.Count; "
        "  end Bad; "
        "  public static method Main(): int "
        "    Bad([1], [2]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kMixedMeet));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kBorrowReturn));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The legal `owned ⊓ owned` meet whose LEFT arm is NULLABLE — §7.7's join
// with a nullable left operand — must still compile and must still discharge
// the selected arm exactly once.
BEZEL_TEST(coalesce_meet_8371_8373,
           legal_owned_meet_nullable_left_disposes_once_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; import System; ") + kWidgetClass +
        "public class C "
        "  public static method Make(f: bool): owned Widget? "
        "    if f then return new Widget(1); end if; "
        "    return null; "
        "  end Make; "
        "  public static method Bad(): owned Widget "
        "    return Make(true) ?? new Widget(2); "
        "  end Bad; "
        "  public static method Main(): int "
        "    using var r := Bad(); "
        "    Console.WriteLine(\"body\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 0);
}

// The BORROWED meet stored into an `owned` FIELD — §28.3's owned-field store
// rule. The `??` spelling hid the borrow from that rule exactly as it hid it
// from the owned-return rule, so the store is now rejected — the SAME verdict
// (and the same diagnostic) the `if`-expression twin of the same meet has
// always had.
BEZEL_TEST(coalesce_meet_8371_8373,
           borrowed_meet_into_owned_field_rejected_8373_28_3) {
    expectRejected(
        "module M; import System; "
        "public class W "
        "  public constructor(n: int) end constructor; "
        "  destructor end destructor; "
        "end W; "
        "public class H "
        "  private owned _f: W? := null; "
        "  public method Store(a: W, b: W) "
        "    _f := a ?? b; "
        "  end Store; "
        "end H; "
        "public class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;",
        kBorrowField);
}

// The STANDALONE collection literal — a collection literal alone is legally
// OWNED (§28.3's collection-literal row) and must keep behaving exactly as
// before: the fix is about the `??` ARM's classification, never about the
// literal's classification in general.
BEZEL_TEST(coalesce_meet_8371_8373,
           standalone_collection_literal_still_owned_28_3) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "public class C "
        "  public static method Main(): int "
        "    using var x: Array<int> := [1]; "
        "    Console.WriteLine(x.Count.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}
