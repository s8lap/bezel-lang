// fix #8179 — a user `extension Object` member named like a stdlib §5 LINQ
// operator is not consulted on a LAZY-SEQUENCE receiver when the call's arity
// differs from every §5 overload of that name (§26.5.1, v0.355).
//
// THE DEFECT. stdlib §5's deferred operators produce a "lazy enumerable": a
// Named TypeRef with no declaration, no astNode and no importedName, carrying
// the element type at genericArgs[0] (`makeLazyEnumerable`,
// src/sema/ExprTypecheck_Calls.cpp). The sentinel carries NO NAME, so §26.5
// rule 2's search ("the compiler searches the current module AND all imported
// modules for extension blocks on `x`'s type, base types, or implemented
// interfaces") related it to NO extension block at all — not to stdlib §5's
// own `IEnumerable<T>` block, and not to the implicit `Object` base ("every
// type derives from Object", §2 / §12). A user's `extension Object` member
// was therefore never a CANDIDATE on a lazy receiver.
//
// MEASURED on this fix's parent (r30-c1 @ 7ea0bb7d), the issue's own probe
// (L1, verbatim):
//
//   extension Object
//     public method Select(): int => 4242;
//   end;
//   ...
//   t := t + a.Where(lambda(x) => x > 0).Select();
//
//   "wrong number of arguments for stdlib §5 LINQ operator `Select`
//    (stdlib §5 `System.Collections.Linq`, v0.286): got 0, expected 1 (#3726)"
//
// — the §5 arity diagnostic, even though §26.5.1 (v0.355) says the §5
// overload is NOT a candidate for that call:
//
//   "For receiver type R, specificity ranks the applicable candidates:
//    extension members of the called name that match the call's arity and
//    argument types, from blocks whose head matches R (step 2 of §26.5), with
//    block-level `where` clauses satisfied by the arguments inferred from the
//    receiver. A `where` clause is an *applicability* filter, never a ranking
//    signal."
//
// A zero-argument `Select` matches no §5 `Select` arity, so the §5 overload
// is not an applicable candidate and the user's `extension Object { Select() }`
// is the only one. §26.5 rule 1 does not block it: a lazy sequence declares
// no instance member `Select`.
//
// WHERE THE FIX IS (and is NOT). The issue's hypothesis named the arity gate
// (`reportLinqArityMismatch`). MEASURED: the extension lookup runs while the
// CALLEE is resolved — BEFORE `typeCall`'s §5 arms run — so the #3726 gate
// fires only BECAUSE the lookup found nothing. The fix is therefore in the
// lookup: the sentinel's implicit `Object` base is consulted for a §5-named
// member whose arity matches no §5 overload.
//
// THE CONTROLS below pin that the fix does not OVER-reach: a §5-named call
// whose arity DOES match a §5 overload still takes the §5 operator (§26.5.1's
// specificity — §5's `IEnumerable<T>` head is more specific than `Object`),
// a genuine §5 arity error with no extension in scope is still rejected with
// the #3726 diagnostic, the eager `Array<int>` receiver is unchanged, and a
// `Dictionary<K,V>`-HEADED extension already answers on a dictionary receiver.
//
// SCOPE — the dictionary half of #8179 is NOT fixed here, and row R5 pins
// why: `extension Object` does not reach a `Dictionary<K,V>` receiver BY
// DESIGN (`builtinHeadReachesObjectExtension`, fix #7775), because the
// dictionary's `Object` view reads freed memory (#7822). MEASURED on this
// fix's parent: `var o: Object := d; o.ToString()` SEGFAULTS on a
// `Dictionary<string,int>`, and the same spelling on a NAMED
// `IEnumerable<int>` aborts under ASan ("allocation-size-too-big"). The lazy
// sentinel's `Object` view is different: `var o: Object := a.Where(f);
// o.ToString()` prints garbage but ASan reports NO error — the lazy value's
// word 0 is the lazy vtable, and it is never a dictionary / set / linked-list.
// So the sentinel is admitted and the dictionary family is not.
//
// THE ORACLE. `runAot` / `runAotChecked` — the strict AOT path
// (fork-isolated `Driver::compileFile` + clang link + native exec). Every row
// asserts the program's STDOUT and its exit code: the program's BEHAVIOUR,
// not "it compiled".
//
// SPEC BASIS. §26.5 rule 2, §26.5.1 *Specificity ordering* (v0.355), §2 /
// §12 (the implicit `Object` base), §8.1 (the universal `Object` members),
// stdlib §5 (`System.Collections.Linq`, v0.286).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §5 arity diagnostic's text (the #3726 wording).
const char* const kWrongArityDiag =
    "wrong number of arguments for stdlib §5 LINQ operator";

// The diagnostic engine has no substring query (only `hasErrors`), so the
// rows that pin a REJECTION read the messages themselves — the 3726 file's
// own `hasError` helper, for the same reason.
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The issue's L1 probe, VERBATIM: the lazy-sequence receiver, the
// `extension Object { Select() }` whose arity matches no §5 `Select`
// overload, and the 100-iteration loop the issue's own text expects to
// print 424200.
std::string lazyReceiverObjectSelect() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Select(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    var t := 0;\n"
        "    for var i in 0..<100 do\n"
        "      t := t + a.Where(lambda(x) => x > 0).Select();\n"
        "    end;\n"
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 1 — the genuine §5 arity error, with NO extension in scope: the
// §5 diagnostic is still the answer. The lazy receiver is the same shape as
// row R1, so this pins that the fix did not silence the gate for a call that
// has no other candidate.
std::string lazyReceiverGenuineArityError() {
    return
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    Console.WriteLine(a.Where(lambda(x) => x > 0).Select());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 2 — the SAME-ARITY call still takes the §5 operator (§26.5.1's
// specificity: stdlib §5's `IEnumerable<T>` head is a subtype of `Object`, so
// the §5 candidate is more specific AND applicable — the lambda argument is
// the `(T) -> int` the §5 signature declares). The extension's `Select` has
// the §5 arity, so it is a candidate too, and §5's wins: the program prints
// the §5 pipeline's element count (2), NOT the extension's 4242 — and the
// extension's `int` result would not even answer `Count()`.
std::string lazyReceiverSameArityStillTakesStdlib() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Select(f: (int) -> int): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    var q := a.Where(lambda(x) => x > 0).Select(lambda(x) => x + 1);\n"
        "    Console.WriteLine(q.Count());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 3 — the EAGER `Array<int>` receiver (the issue's own reviewer
// control p47): the same `extension Object { Select() }` already answered
// there before this fix, and must keep answering.
std::string arrayReceiverObjectSelect() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Select(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    Console.WriteLine(a.Select());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R5's program — the issue's L5 probe: a `Dictionary<string,int>`
// receiver and an `extension Object` that names two §5 operators.
std::string dictionaryReceiverObjectSelect() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Select(): int => 4242;\n"
        "  public method OrderBy(): int => 4343;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add(\"a\", 1);\n"
        "    Console.WriteLine(d.Select());\n"
        "    Console.WriteLine(d.OrderBy());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 4 — a `Dictionary<K,V>`-HEADED extension (the spelling that DOES
// apply to a dictionary receiver) answers on a dictionary receiver, and the
// §5 arity diagnostic does not preempt it.
std::string dictionaryHeadExtensionSelect() {
    return
        "module M;\n"
        "extension Dictionary<K,V>\n"
        "  public method Select(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add(\"a\", 1);\n"
        "    Console.WriteLine(d.Select());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// R1 — THE BUG. The issue's L1 probe, verbatim: a §5-named user extension
// whose arity matches no §5 overload is the only applicable candidate on a
// lazy-sequence receiver, so the call is the user's and prints 4242 per call
// (424200 over the issue's 100 iterations).
//
// BASE (r30-c1 @ 7ea0bb7d): a COMPILE ERROR —
// "wrong number of arguments for stdlib §5 LINQ operator `Select` …
//  got 0, expected 1 (#3726)".
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_extension_arity_fallthrough_8179,
           lazy_receiver_object_select_answers_the_extension) {
    // `runAot`, not `runAotChecked`: the issue's literal repro passes an OWNED
    // lazy receiver to the extension, and the extension does not dispose it —
    // §28.3's set-erasing rule makes the extension own the receiver, so the
    // pipeline leaks (MEASURED under LSan via `scripts/agent_check.sh --probe
    // --asan`: 61 600 B in 500 allocations over the 100 iterations). That is
    // the PRE-EXISTING "an owned temporary receiver passed to ANY extension
    // leaks" class (the r39-rev-7967 review's L3c residual), not a defect of
    // this fix; the row therefore pins the VALUE (424200), and the leak is
    // reported rather than pinned.
    JitResult r = runAot(lazyReceiverObjectSelect());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "424200\n");
}

// ---------------------------------------------------------------------------
// R2 — CONTROL 1. The genuine §5 arity error with NO extension in scope is
// still rejected with the #3726 diagnostic: the fix does not silence the
// gate for a call that has no other candidate.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_extension_arity_fallthrough_8179,
           genuine_arity_error_without_an_extension_is_still_rejected) {
    JitResult r = runAot(lazyReceiverGenuineArityError());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kWrongArityDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "got 0, expected 1"));
    BEZEL_EXPECT_TRUE(hasError(r, "(#3726)"));
}

// ---------------------------------------------------------------------------
// R3 — CONTROL 2. The SAME-ARITY call still takes the §5 operator
// (§26.5.1's specificity): the extension's `Select` has the §5 arity, so it
// is a candidate too, and §5's more specific `IEnumerable<T>` head wins. The
// program prints the §5 pipeline's element count (2) — the extension's 4242
// would not even answer `Count()`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_extension_arity_fallthrough_8179,
           same_arity_call_still_takes_the_stdlib_operator) {
    JitResult r = runAotChecked(lazyReceiverSameArityStillTakesStdlib());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ---------------------------------------------------------------------------
// R4 — CONTROL 3. The eager `Array<int>` receiver (the issue's reviewer
// control p47) is unchanged: the same extension answers there.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_extension_arity_fallthrough_8179,
           array_receiver_object_select_is_unchanged) {
    JitResult r = runAotChecked(arrayReceiverObjectSelect());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R5 — the DICTIONARY half of #8179, NOT fixed here, and the reason it is not.
// `extension Object` does not reach a `Dictionary<K,V>` receiver BY DESIGN
// (`builtinHeadReachesObjectExtension`, fix #7775): the dictionary's `Object`
// view reads freed memory (#7822) — MEASURED: `var o: Object := d;
// o.ToString()` SEGFAULTS on a `Dictionary<string,int>`, and the same
// spelling on a NAMED `IEnumerable<int>` aborts under ASan. Admitting the
// dictionary family would turn today's compile error into a run-time fault,
// so the §5 arity diagnostic stays the answer there — and the issue's
// finding is reported instead.
//
// (The LAZY sentinel's `Object` view is different, and that is why row R1 is
// fixed: `var o: Object := a.Where(f); o.ToString()` prints garbage but ASan
// reports NO error — the lazy value's word 0 is the lazy vtable, and it is
// never a dictionary / set / linked-list.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_extension_arity_fallthrough_8179,
           dictionary_receiver_object_select_stays_rejected) {
    JitResult r = runAot(dictionaryReceiverObjectSelect());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kWrongArityDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "got 0, expected 1"));
    BEZEL_EXPECT_TRUE(hasError(r, "(#3726)"));
}

// ---------------------------------------------------------------------------
// R6 — CONTROL 4. A `Dictionary<K,V>`-HEADED extension — the spelling that
// DOES apply to a dictionary receiver — answers, and the §5 arity diagnostic
// does not preempt it. This is the user-visible answer the dictionary half
// of #8179 has today.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_extension_arity_fallthrough_8179,
           dictionary_headed_extension_answers_on_a_dictionary_receiver) {
    JitResult r = runAotChecked(dictionaryHeadExtensionSelect());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}
