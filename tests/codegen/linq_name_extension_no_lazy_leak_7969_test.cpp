// fix #7969 (§26.5 rules 1-2 / §26.5.1, v0.355): a USER EXTENSION named like
// a stdlib §5 LINQ operator LEAKED the §5 lazy source.
//
// THE DEFECT. stdlib §5's LINQ operators are LAZY: the compiler lowers
// `a.Where(p)` / `a.Select(f)` / … by MATERIALISING the receiver into a lazy
// enumerator (`bz_lazy_from_array` / `bz_lazy_from_llist` / …) and building a
// pipeline over it. The lowering is reached by NAME — `tryLowerLazyLinqCall`'s
// `isOp` gate is a list of §5 operator names — and it materialises the
// receiver BEFORE the call's CHOSEN CALLEE is consulted.
//
// A user extension named like a §5 operator is a §26.5 rule 2 candidate, and
// rule 3 / §26.5.1 select it when it is applicable and more specific than the
// §5 block (e.g. `extension Array<T> { Select() }`, whose substituted head
// `Array<int>` is a subtype of the §5 block's `IEnumerable<int>`). Such a call
// reached the §5 lazy arm, the receiver was materialised into a lazy source,
// the arm then found no §5 shape to lower (the extension's arity is 0; §5's
// `Select` takes 1) and returned null — so the extension's own lowering ran
// and the materialised lazy source was LEAKED.
//
// MEASURED on this fix's parent (r30-c1 @ ff2855bf), `bezelc
// --sanitize=address` on the issue's own probes:
//
//   shape                                        base leak
//   extension Array<T> { Select() }, 100 calls   27 876 B in 202 allocations
//   extension Object  { Select() }, 1 call         276 B in 2 allocations
//
// THE FIX. The discriminator is SEMA'S CHOSEN CALLEE, not the name: when sema
// bound the call to an extension member, that member IS the answer (§26.5
// rules 2-3), and the §5 lazy arm declines BEFORE any receiver IR is emitted
// (§22.5: an operand is evaluated once; §26.7: the receiver is evaluated
// once) — so the leak goes away rather than merely shrinking. §26.5 rule 1's
// commitment is respected: a name the receiver's own DECLARED SURFACE
// declares keeps the §5 arm.
//
// THE CONTROLS below pin that the built-in §5 operators still work (a lambda
// argument's `Select` is the §5 pipeline), and that a user extension with a
// DIFFERENT name still answers.
//
// THE ORACLE. `runAotChecked` — the strict AOT path (fork-isolated
// `Driver::compileFile` + clang link + native exec) with generated-code
// AddressSanitizer AND LeakSanitizer always on. A leak makes the RUN fail, so
// `BEZEL_EXPECT_TRUE(r.ran)` is the assertion that turns the leak into a red
// row (no `leaksAtLeast`: a leak oracle would invert the moment the leak is
// fixed). The leak rows are LOOPED (100 iterations) because a one-shot leak
// repro can hide under the sanitizer's noise.
//
// SPEC BASIS. §26.5 rules 1-3 (v0.355), §26.5.1 *Specificity ordering*
// (v0.355), §22.5 / §26.7 (an operand is evaluated once), stdlib §5 (the
// LINQ operators are lazy and materialise their source).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's probe A: `extension Array<T> { Select() }` — a §26.5.1 winner
// over the §5 block (the substituted head `Array<int>` is a subtype of the
// §5 block's `IEnumerable<int>`). The call is LOOPED 100 times, because a
// one-shot leak repro can hide under the sanitizer's noise: on the base this
// is 27 876 B in 202 allocations.
std::string arrayExtensionSelectLoop() {
    return
        "module M;\n"
        "extension Array<T>\n"
        "  public method Select(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    for var i in 0..<100 do\n"
        "      if a.Select() <> 4242 then Console.WriteLine(\"bad\"); end;\n"
        "    end;\n"
        "    Console.WriteLine(a.Select());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The issue's probe B: `extension Object { Select() }` — a §26.5 rule 2
// candidate that wins because §5 declares NO `Select()` at arity 0 (the §5
// `Select` takes a selector), so the §5 block is not a candidate for this call
// at all. On the base this leaks 276 B in 2 allocations.
std::string objectExtensionSelect() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Select(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    Console.WriteLine(a.Select());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL — the built-in §5 `Select` still lowers the lazy pipeline (the
// §5 arm must keep answering where §26.5 rule 1 / rule 3 leave it the
// answer). `a.Select(lambda(x) => x + 1)` keeps 2 elements.
//
// BASE: also 2 — this row must NOT change.
std::string builtinSelectStillLowers() {
    return
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    var s := a.Select(lambda(x) => x + 1);\n"
        "    Console.WriteLine(s.Count());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL — a user extension with a DIFFERENT name still answers, and the
// §5 arm is not involved.
//
// BASE: also 4242 — this row must NOT change.
std::string differentlyNamedExtensionStillWorks() {
    return
        "module M;\n"
        "extension Array<T>\n"
        "  public method Total(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    Console.WriteLine(a.Total());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// R1 — the issue's probe A: an `extension Array<T> { Select() }` called 101
// times. The §5 lazy arm must decline BEFORE materialising the receiver, so
// no `bz_lazy_from_array` is built at all.
//
// BASE (r30-c1 @ ff2855bf): LeakSanitizer reports 27 876 byte(s) leaked in
// 202 allocation(s) — the row fails the RUN, so `r.ran` is false.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_name_extension_no_lazy_leak_7969,
           array_extension_select_does_not_leak_the_lazy_source) {
    JitResult r = runAotChecked(arrayExtensionSelectLoop());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R2 — the issue's probe B: an `extension Object { Select() }` called once.
//
// BASE: LeakSanitizer reports 276 byte(s) leaked in 2 allocation(s).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_name_extension_no_lazy_leak_7969,
           object_extension_select_does_not_leak_the_lazy_source) {
    JitResult r = runAotChecked(objectExtensionSelect());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// C1 — CONTROL: the built-in §5 `Select` still lowers the lazy pipeline.
//
// BASE: also 2 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_name_extension_no_lazy_leak_7969,
           builtin_select_still_lowers_the_lazy_pipeline) {
    JitResult r = runAotChecked(builtinSelectStillLowers());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ---------------------------------------------------------------------------
// C2 — CONTROL: a user extension with a DIFFERENT name still answers.
//
// BASE: also 4242 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_name_extension_no_lazy_leak_7969,
           differently_named_extension_still_works) {
    JitResult r = runAotChecked(differentlyNamedExtensionStillWorks());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}
