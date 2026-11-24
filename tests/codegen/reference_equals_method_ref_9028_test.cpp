// Codegen tests for issue #9028 (v0.359) — §8.1 / §8.4 / §11.7 / §22.12 /
// §27.6: `ReferenceEquals` spelled through a CLASS type name and USED AS A
// VALUE (a §27.6 method-group reference).
//
// §8.1 declares the root `Object`'s surface, and `ReferenceEquals` is its ONE
// static:
//
//     public class Object
//       ...
//       public static method ReferenceEquals(a: Object?, b: Object?): bool;
//     end Object;
//
// §11.7: "`Object`'s static `ReferenceEquals` (§8.1) is inherited by a static
// class as by every class"; §22.12: "A static member declared on a base class
// is a member of every class derived from it ... and may be named through a
// derived type". So `K.ReferenceEquals` names the INHERITED static — and
// §27.6 turns a static named in a VALUE position into a function value, the
// `(Object, Object) -> bool` pair the root spelling forms:
//
//     var f := K.ReferenceEquals;   // §27.6 method-group reference
//     f(k, k2);                      // the root's identity compare (§8.4)
//
// WHAT THE DEFECT WAS. Sema typed the READ form (the #8795 arm) as a
// `(Object, Object) -> bool` function value, but codegen had NO lowering for
// it: the primitive-intrinsic method-reference path recognized only a
// PRIMITIVE-keyword target (`Object.ReferenceEquals` — `Object` is a
// primitive), so a CLASS type name fell through CG60 to `ret nullptr`. The
// local's slot then degraded to a zero-initialized value behind #2434's
// "initializer expression failed to lower" warning, and the call of that
// zero-init closure was a null-function SEGV (measured: rc 139, ASan
// "SEGV on unknown address 0x000000000008").
//
// The rows below assert the program's STDOUT — which is what the degradation
// erased — plus the exit code, and the checked oracle asserts the sanitizer
// leg really ran (a toolchain skip fails the row instead of passing it
// silently) and that LeakSanitizer counted nothing.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The ISSUE'S REPRO, verbatim: the class type name spelling the inherited
// static, taken as a VALUE (§27.6) and called. `k` and `k2` are two DISTINCT
// instances, so the identity compare is `False` (§8.4); `k` against itself is
// `True`.
std::string issueRepro() {
    return "module M; class K "
           "end K; "
           "class C "
           "  public static method Main(): int "
           "    var k := new K(); "
           "    var k2 := new K(); "
           "    var f := K.ReferenceEquals; "
           "    Console.WriteLine(f(k, k2)); "
           "    Console.WriteLine(f(k, k)); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The diagnostic text of the result, for the leak rows' assertions.
std::string diagnosticText(const bezel::test::JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes.
void assertNoLeak(const bezel::test::JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes,
                                                   leakedAllocs);
    BEZEL_EXPECT_EQ(sawSummary ? leakedAllocs : 0ull, 0ull);
    BEZEL_EXPECT_EQ(sawSummary ? leakedBytes : 0ull, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

}  // namespace

// #9028 / §8.1 / §8.4 / §11.7 / §22.12 / §27.6: the class-type-name spelling
// of the inherited static, taken as a VALUE, answers the identity compare.
// On the base it crashed (the #2434 zero-init + the null-closure SEGV).
BEZEL_TEST(reference_equals_method_ref_9028,
           class_type_name_method_ref_identity_answers) {
    auto r = runAot(issueRepro());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n");
}

// #9028 / §11.7 / §22.12 / §27.6: the ROOT spelling of the same reference
// form — the twin that must keep answering the same thing (the two spellings
// now share the ONE identity compare).
BEZEL_TEST(reference_equals_method_ref_9028,
           root_method_ref_identity_answers) {
    auto r = runAot(
        "module M; class K "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var k2 := new K(); "
        "    var f := Object.ReferenceEquals; "
        "    Console.WriteLine(f(k, k2)); "
        "    Console.WriteLine(f(k, k)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n");
}

// #9028 / §14.1 / §22.12 / §27.6: the GENERIC spelling — a generic class's
// type name with its type arguments (§22.12: "a generic type in receiver
// position supplies the type arguments") taken as a VALUE.
//
// NOT COVERED HERE (a separate defect, filed as #9042): the generic
// spelling's READ form is left UNTYPED by sema — the #8795 sema arm types the
// READ form only when the receiver's own exprType is absent (the plain
// class-type-name spelling), and a generic type name carries an INSTANTIATED
// receiver type, so sema takes the value-receiver path and the access stays
// untyped. It therefore degrades to the #3079 "failed to lower" (nothing
// printed) rather than to the null-closure SEGV this issue is about. That is
// a SEM A gap, not a codegen one: no codegen lowering can be reached for an
// expression sema left untyped.

// #9028 / §8.1 / §8.4 / §28.9: the method-reference form called 200 times,
// under the CHECKED oracle (the sanitizer leg really runs — a toolchain skip
// fails the row instead of passing it silently). The identity compare itself
// allocates nothing (§8.6: a reference-typed operand IS its own box), so the
// leak oracle is the guard that the new spelling did not buy an allocation —
// and the 200-iteration loop is the guard that the reference's +1 closure cell
// is released at scope end (§28.9) instead of leaking one cell per reference.
BEZEL_TEST(reference_equals_method_ref_9028,
           class_type_name_method_ref_loop_200_no_leak) {
    auto r = runAotChecked(
        "module M; class K "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var k2 := new K(); "
        "    var f := K.ReferenceEquals; "
        "    var hits := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      if f(k, k2) then hits := hits + 1; end; "
        "      if f(k, k) then hits := hits + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(hits); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
    assertNoLeak(r);
}

// #9028 / §27.6 / §28.9: the reference taken INSIDE the loop — 200 fresh +1
// closure cells, one per iteration, each released at the end of its own scope.
// This is the row that pins the producer/consumer balance: the reference's
// cell must not survive its iteration (§28.9), and the identity compare must
// not buy an allocation of its own (§8.6). Same CHECKED oracle.
BEZEL_TEST(reference_equals_method_ref_9028,
           class_type_name_method_ref_taken_in_loop_200_no_leak) {
    auto r = runAotChecked(
        "module M; class K "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var k2 := new K(); "
        "    var hits := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f := K.ReferenceEquals; "
        "      if f(k, k2) then hits := hits + 1; end; "
        "      if f(k, k) then hits := hits + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(hits); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
    assertNoLeak(r);
}
