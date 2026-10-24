// fix #7098 (spec §27.6 / §5.1, v0.343): a bare method-group REFERENCE to a
// member of the array-backed BORROWING LIST interface `IList<T>` formed at a
// TARGET function type that does not match the member's own signature.
//
// THE DEFECT. §27.6 (v0.343): "The reference's signature must be compatible
// with the target function type: its parameter and return types unify with the
// target's under the standard implicit-conversion rules (§5.1)."
// `IList<T>.Contains` is declared `Contains(item: T) -> bool` (stdlib §4.1),
// so `il.Contains` over an `IList<int>` forms `(int) -> bool`. The target
// `(long) -> bool` is NOT reachable from it: `long` does not convert to `int`
// under §5.1, so the formation is a compile error.
//
// MEASURED on the staging tip (the #7014 arm, before this fix): the program
// COMPILED (rc 0, with only the internal #2434 degradation warning "the
// initializer expression failed to lower to a value for this local's slot")
// and then SEGFAULTED — run rc 139, and under ASan "SEGV on unknown address
// 0x000000000008 … The signal is caused by a READ memory access … rax =
// 0x0000000000000008". The #7014 arm `return nullptr`'d on the mismatch: a
// SILENT non-lowering, not a diagnostic. A program that COMPILES and then
// SEGFAULTS is strictly worse than a rejection.
//
// THE ORDINARY §27.6 REFERENCE IS REJECTED FOR THE SAME MISMATCH (measured on
// the same tip): over a user class's `N(x: int): bool`, the target
// `(long) -> bool` is rejected by sema with "the initializer for `f` has a
// type that is not implicitly convertible to the target type (§5.1)". The
// `IList<T>` arm has NO MethodSymbol (an interface head has no method
// DECLARATION to bind), so sema never saw the formation and the codegen arm
// had to refuse it — now it does, with a located §27.6 diagnostic.
//
// THE TWO VERDICTS. §27.6 also says "formation may bridge representation-
// changing conversions by synthesizing an adapter at the reference site": the
// target `(byte) -> bool` over `Contains(item: int)` is a §5.1-convertible
// difference (`byte` converts to `int`), so the ordinary shape ADMITS it (the
// adapter widens the argument). This compiler does not synthesize that
// adapter for an `IList<T>` member reference (the #5208 formation-site adapter
// is wired to a MethodSymbol's thunk, and an interface head has none), so the
// formation is refused — LOUDLY, with the remedy — rather than minted at an
// ABI the call site would violate (which is the #7098 SEGFAULT).
//
// THE ROWS. The rejection rows assert the DIAGNOSTIC TEXT (never an exit
// code alone — per tests/README.md a genuine compile-time rejection must be
// distinguished by its message), and the control rows assert the OBSERVABLE
// behaviour of the legal shapes.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// Per tests/README.md: a genuine compile-time rejection must be distinguished
// by asserting on the diagnostic TEXT — never by exit code or `!r.ran` alone.
bool hasErrorContaining(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

// The §27.6 formation diagnostic's own wording.
const char* kNotCompatible = "not compatible with the declared type";
const char* kSection = "§27.6";

}  // namespace

// THE ISSUE'S OWN REPRO (spec §27.6 / §5.1, v0.343): the group formed at a
// target function type the member's own signature does not reach. The
// formation must be REJECTED with a located §27.6 diagnostic — the pre-fix
// build compiled it (rc 0) and the call SEGFAULTED (rc 139).
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_contains_group_at_an_incompatible_target_is_rejected) {
    auto r = runAot(
        "module M; "
        "import System; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var f: (long) -> bool := il.Contains; "
        "  Console.WriteLine(f(1L)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(kNotCompatible) != std::string::npos &&
                d.message.find(kSection) != std::string::npos)
            sawError = true;
    }
    BEZEL_EXPECT_TRUE(sawError);
}

// The same rejection, reached through the OTHER member signatures the
// `IList<T>` head declares (stdlib §4.1: `Add(item: T)`, `Clear()`,
// `Contains(item: T)`, `Remove(item: T)`, `Insert(index: int, item: T)`) —
// an ARITY mismatch is incompatible on §27.1.1's own terms ("the rule is
// slot-wise over equal parameter counts").
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_group_at_an_arity_mismatched_target_is_rejected) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "import System; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var f: (int, int) -> bool := il.Contains; "
        "  Console.WriteLine(f(1,2)); "
        "  return 0; "
        "end Main; end C;",
        kNotCompatible));
}

// The RETURN slot is incompatible too: `Contains` returns `bool`, and §5.1
// admits no conversion between `bool` and an integer ("Between integers and
// `bool`" is never implicit).
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_group_at_an_incompatible_return_target_is_rejected) {
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        "module M; "
        "import System; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var f: (int) -> int := il.Contains; "
        "  Console.WriteLine(f(1)); "
        "  return 0; "
        "end Main; end C;",
        kNotCompatible));
}

// The §27.6 CONVERTIBLE case: the target `(byte) -> bool` over
// `Contains(item: int)` is a §5.1-convertible difference (`byte` converts to
// `int`), so §27.6 admits it through a formation-site adapter — which this
// compiler does not build for an `IList<T>` member reference. The formation is
// refused LOUDLY (with the remedy), NOT silently non-lowered: the pre-fix
// build compiled it and the call SEGFAULTED (rc 139), exactly as the
// incompatible case did.
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_group_at_a_convertible_target_is_refused_loudly) {
    auto r = runAot(
        "module M; "
        "import System; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var f: (byte) -> bool := il.Contains; "
        "  Console.WriteLine(f(1)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find("formation-site adapter") != std::string::npos)
            sawError = true;
    }
    BEZEL_EXPECT_TRUE(sawError);
}

// CONTROL — the target that IS the member's own signature still forms and
// answers exactly as the call does (stdlib §4.1: `Contains(item: T) -> bool`).
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_contains_group_at_the_member_signature_still_works) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f: (int) -> bool := il.Contains; "
            "  Console.WriteLine(f(2)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}

// CONTROL — the UNANNOTATED formation (no target type to be incompatible
// with) still takes the member's own signature as its formed type (§27.6:
// "the callee's own signature read as a function type").
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_contains_group_without_an_annotation_still_works) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var f := il.Contains; "
            "  Console.WriteLine(f(2)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}

// CONTROL — the CALL form of the same member is untouched: it still answers
// `True`.
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_call_form_still_works) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  Console.WriteLine(il.Contains(2)); "
            "  return 0; "
            "end Main; end C;"),
        "True");
}

// CONTROL — the OTHER `IList<T>` members keep forming at their own signatures
// (stdlib §4.1: `Add(item: T)`, `Clear()`, `Remove(item: T)`,
// `Insert(index: int, item: T)`), and the group still mutates the list.
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_27_6_ilist_other_members_still_form_at_their_own_signatures) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "import System; "
            "class C public static method Main(): int "
            "  var a: Array<int> := [1,2,3]; "
            "  var il: IList<int> := a; "
            "  var add: (int) -> () := il.Add; "
            "  var rem: (int) -> bool := il.Remove; "
            "  add(4); "
            "  Console.WriteLine(rem(2)); "
            "  Console.WriteLine(il.Count); "
            "  return 0; "
            "end Main; end C;"),
        "True\n3");
}

// The MEMORY oracle on the legal shape: the closure cell the group is backed
// by is released at scope end (§28.9) and the array is freed exactly once —
// no leak, no double free. `runAotChecked` returns ran=false on any ASan /
// LSan fire.
BEZEL_TEST(builtin_list_member_ref_7098,
           spec_28_9_ilist_group_is_memory_clean) {
    auto r = runAotChecked(
        "module M; "
        "import System; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var f: (int) -> bool := il.Contains; "
        "  Console.WriteLine(f(2)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
