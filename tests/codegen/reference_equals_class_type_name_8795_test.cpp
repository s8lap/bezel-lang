// Codegen tests for issue #8795 (v0.359) — §8.1 / §22.12 / §2.7 rule 1 /
// §12 / §11.7: `ReferenceEquals` spelled through a CLASS type name.
//
// §8.1 declares the root `Object`'s surface, and `ReferenceEquals` is its ONE
// static:
//
//     public class Object
//       ...
//       public static method ReferenceEquals(a: Object?, b: Object?): bool;
//     end Object;
//
// §12: "If no base class is specified, the implicit base is `Object`" — so
// every class derives from `Object` (§8.1: "classes ... directly through the
// inheritance chain"), and §11.7 says it outright for the static: "`Object`'s
// static `ReferenceEquals` (§8.1) is inherited by a static class as by every
// class". §2.7 rule 1 resolves a chain head that is a TYPE against "that
// type's members, declared or inherited (... a static member ...; an
// inherited static per §22.12)", and §22.12: "A static member declared on a
// base class is a member of every class derived from it ... and may be named
// through a derived type".
//
// So `K.ReferenceEquals(a, b)` names the INHERITED static and is the same
// call as `Object.ReferenceEquals(a, b)` — the receiver's type does not enter
// the comparison (§8.4: "compares object identity").
//
// RED on the base: the class-type-name spelling compiled with only the #3079
// "failed to lower" warning and PRINTED NOTHING (the call's argument never
// lowered, so the whole `Console.WriteLine` statement emitted nothing). The
// rows below assert the program's STDOUT, which is what the degradation
// erased — an assertion on the absence of the warning would be vacuous
// (#3138: `runAot` fills diagnostics only on a reject).

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The ISSUE'S REPRO, verbatim: a class type name spelling the inherited
// static. `k` and `k2` are two DISTINCT instances, so the identity compare is
// `False` (§8.4).
std::string issueRepro() {
    return "module M; class K "
           "end K; "
           "class C "
           "  public static method Main(): int "
           "    var k := new K(); "
           "    var k2 := new K(); "
           "    Console.WriteLine(K.ReferenceEquals(k, k2)); "
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

// #8795 / §8.1 / §22.12: the class-type-name spelling of the inherited
// static prints the identity compare's answer. On the base it printed
// NOTHING (the #3079 degradation).
BEZEL_TEST(reference_equals_class_type_name_8795,
           class_type_name_distinct_instances_false) {
    auto r = runAot(issueRepro());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// #8795 / §8.4: the same spelling on the SAME instance is `True` — the row
// that keeps a wrong fix from answering `false` for everything.
BEZEL_TEST(reference_equals_class_type_name_8795,
           class_type_name_same_instance_true) {
    auto r = runAot(
        "module M; class K "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    Console.WriteLine(K.ReferenceEquals(k, k)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// #8795 / §14.1 / §22.12: the GENERIC spelling — a generic class's type name
// with its type arguments (§22.12: "a generic type in receiver position
// supplies the type arguments"). Same inherited static, same answer.
BEZEL_TEST(reference_equals_class_type_name_8795,
           generic_class_type_name_distinct_instances_false) {
    auto r = runAot(
        "module M; class G<T> "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    var g := new G<int>(); "
        "    var g2 := new G<int>(); "
        "    Console.WriteLine(G<int>.ReferenceEquals(g, g2)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// #8795 / §8.1 / §8.4 / §28.9: the same call, 200 times, under the CHECKED
// oracle (the sanitizer leg really runs — a toolchain skip fails the row
// instead of passing it silently). The identity compare itself allocates
// nothing (§8.6: a reference-typed operand IS its own box), so the leak
// oracle is the guard that the new spelling did not buy an allocation.
BEZEL_TEST(reference_equals_class_type_name_8795,
           class_type_name_loop_200_no_leak) {
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
        "      if K.ReferenceEquals(k, k2) then hits := hits + 1; end; "
        "      if K.ReferenceEquals(k, k) then hits := hits + 1; end; "
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
