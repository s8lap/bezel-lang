// Regression tests for issue #9216 — a BARE instance method group of the
// enclosing class (`var f: () -> int := Get;`) formed its closure cell and
// never registered it for release: 72 B leaked per formation (a 64 B cell +
// an 8 B receiver-capture env block). The QUALIFIED twin (`this.Get`) was
// leak-clean on the same tree.
//
// SPEC — settled, not underspecified.
//
//   §27.6 *Method references* (v0.359): "Method references work for static
//   methods, instance methods (the instance is captured at formation — by
//   reference for class- or interface-typed receivers, by copy for
//   value-shaped receivers …), properties, and extension methods." The BARE
//   spelling of an enclosing-class instance method is the same reference the
//   QUALIFIED spelling `this.M` forms: §2.7's bare-name lookup finds the
//   enclosing type's own member, and the receiver is the implicit `this`.
//
//   §28.9 *Runtime-managed types* (v0.359): a function-typed value "is
//   runtime-managed … the type's lifetime is managed by the runtime without
//   user involvement", and "Runtime management governs the *function value
//   itself* — the heap-allocated closure object that backs it". So the cell
//   the formation allocates is reclaimed exactly once.
//
// MEASURED PRE-FIX (r30-c1 @ d3f56e723, the base), `bezelc --sanitize=address`
// (LeakSanitizer at exit):
//
//   var f: () -> int := Get;              -> Direct leak of 64 byte(s)
//                                           + Indirect leak of 8 byte(s)
//                                           = "SUMMARY: AddressSanitizer:
//                                             72 byte(s) leaked in 2
//                                             allocation(s)."
//   a 300-iteration loop of the same      -> "21600 byte(s) leaked in 600
//                                             allocation(s)" (300 x 72 B)
//   this.cb := Get;   (a field store)     -> 72 B in 2 allocations
//   Apply(Get);       (a call argument)   -> 72 B in 2 allocations
//   var f := Get;     (the un-ascribed    -> 72 B in 2 allocations
//                       spelling)
//
// The program's own OUTPUT was correct throughout (the cell works; only its
// lifetime was wrong).
//
// THE ROOT CAUSE. The formation sites are `Emitter::lowerExprInner`'s two
// method-reference arms (src/codegen/LlvmBackend_Exprs.cpp): the qualified
// one (`makeMethodThunk(emitMs, recv, recvTy, &e)`) and the bare one, which
// forms a cell for a static group AND — through the #880/#881 path — for an
// INSTANCE group of the enclosing class, bound to the implicit `this`
// (`makeMethodThunk(emitMs, currentThis_, nullptr, &e)`).
//
// The DISCHARGE side is driven by ONE shared predicate,
// `Emitter::isPlusOneClosureProducer` (src/codegen/LlvmBackend_Setup.cpp).
// Its bare-`IdentifierExpr` arm admitted only a STATIC method, on a stale
// reading of the emitter's gate ("a bare IdentifierExpr method reference is
// lowered to a cell ONLY for a static method") that the #880/#881 instance
// arm had already made false. Every discharge site asks that predicate, so a
// "not a producer" answer left the cell unowned:
//   * the scope-end release of a function-typed LOCAL
//     (`collectClosureReleaseEligible`'s candidate walk) was never armed;
//   * `lowerClosureFieldStore`'s retain-vs-transfer decision RETAINED the
//     field's own reference and left the producer's +1 un-dropped;
//   * the call-site release of a producer ARGUMENT never fired.
//
// THE FIX. `isPlusOneClosureProducer`'s bare-`IdentifierExpr` arm now admits
// the INSTANCE method group as well, restoring the agreement between the
// predicate and the emitter.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row
// asserts the program's own output AND that LeakSanitizer counted nothing,
// so a silent zero-fill cannot pass and no double free can hide. `runAot`
// alone CANNOT be the oracle here: it does not run the AddressSanitizer pass
// over the generated module (only `runAotChecked` sets `opts.sanitizeAddress`),
// so the leak would go unreported and every row would be green on the base —
// pinning nothing. The rows LOOP (200 iterations inside the program), so the
// leak cannot hide under the sanitizer's noise. This is the same oracle the
// neighbouring ownership leak rows use
// (tests/ownership/conditional_method_group_closure_release_9170_test.cpp,
// tests/ownership/bare_method_group_local_call_9155_test.cpp).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// The program under test. `classHead` is the class's header (its base /
// interface list), `members` its extra members, and `mainBody` the body of
// `Main`. Every row runs `Main`, which loops `iterations` times over a FRESH
// instance — a fresh receiver, a fresh cell.
std::string program(const std::string& classHead, const std::string& members,
                    const std::string& mainBody) {
    return R"BZL(
module M;
public class P)BZL" + classHead + R"BZL(
)BZL" + members + R"BZL(
  public static method Main(): int
)BZL" + mainBody + R"BZL(
  end Main;
end P;
)BZL";
}

// `Main`'s body: 200 iterations, each over a fresh instance, each summing the
// instance method's answer.
std::string loopBody(int iterations) {
    return "    var total := 0;\n"
           "    for var j in 0..<" + std::to_string(iterations) + " do\n"
           "      var p := new P();\n"
           "      total := total + p.Make();\n"
           "    end;\n"
           "    Console.WriteLine(total);\n"
           "    return 0;\n";
}

// The bare instance method group bound to an ASCRIBED local — the issue's own
// repro, amplified. The local's declared type is the formed function type
// `() -> int` (§27.6: the receiver slot removed for a bound reference).
const char* kAscribedLocal =
    "  public method Get(): int => 5;\n"
    "  public method Make(): int\n"
    "    var f: () -> int := Get;\n"
    "    return f();\n"
    "  end Make;\n";

// The UN-ASCRIBED local: `var f := Get;` — the spelling issue #9155 made
// reach the formation at all (before it, the base dropped the call to
// nothing).
const char* kUnascribedLocal =
    "  public method Get(): int => 5;\n"
    "  public method Make(): int\n"
    "    var f := Get;\n"
    "    return f();\n"
    "  end Make;\n";

// The FIELD-STORE spelling: the group is stored into a function-typed field of
// the enclosing class, so the field owns the cell and the producer's +1 must
// be TRANSFERRED (not retained) — the discipline `lowerClosureFieldStore`
// decides with the same predicate.
const char* kFieldStore =
    "  private cb: () -> int;\n"
    "  public method Get(): int => 5;\n"
    "  public constructor()\n"
    "    this.cb := Get;\n"
    "  end constructor;\n"
    "  public method Make(): int => cb();\n";

// The BARE STATIC group of the enclosing class — the shape issue #9155
// landed. It is a producer the predicate already recognised, so it must stay
// clean (a narrowing fix that dropped the static case would leak it).
const char* kBareStatic =
    "  static method P(): int => 5;\n"
    "  public method Make(): int\n"
    "    var f := P;\n"
    "    return f();\n"
    "  end Make;\n";

// The QUALIFIED twin of the first row — `this.Get` instead of the bare `Get`.
// It was clean on the base and must stay clean: the fix must not disturb the
// path that already released the cell.
const char* kQualifiedLocal =
    "  public method Get(): int => 5;\n"
    "  public method Make(): int\n"
    "    var f: () -> int := this.Get;\n"
    "    return f();\n"
    "  end Make;\n";

}  // namespace

// ===========================================================================
// ROW 1 — the issue's own repro, amplified 200x: an ASCRIBED local bound to
// the bare instance method group of the enclosing class.
//
// RED on the base: 72 B leaked per formation (200 formations ⇒ 14 400 B in
// 400 allocations). GREEN with the fix: prints 1000, exits 0, leaks nothing.
// ===========================================================================
BEZEL_TEST(bare_instance_method_group_closure_release_9216,
           ascribed_local_bare_instance_group_no_leak_spec_28_9) {
    expectClean(runAotChecked(program("", kAscribedLocal, loopBody(200))), 0,
                "1000\n");
}

// ===========================================================================
// ROW 2 — the UN-ASCRIBED spelling `var f := Get;` (§27.6's formed type is
// inferred from the initializer). Same root cause, same leak.
//
// RED on the base: 72 B per formation. GREEN with the fix.
// ===========================================================================
BEZEL_TEST(bare_instance_method_group_closure_release_9216,
           un_ascribed_local_bare_instance_group_no_leak_spec_28_9) {
    expectClean(runAotChecked(program("", kUnascribedLocal, loopBody(200))), 0,
                "1000\n");
}

// ===========================================================================
// ROW 3 — the FIELD-STORE spelling `this.cb := Get;`. The field owns the
// cell, so the producer's +1 is TRANSFERRED to the field and released once,
// by the field's own disposal.
//
// RED on the base: 72 B per formation. GREEN with the fix.
// ===========================================================================
BEZEL_TEST(bare_instance_method_group_closure_release_9216,
           field_store_bare_instance_group_no_leak_spec_28_9) {
    expectClean(runAotChecked(program("", kFieldStore, loopBody(200))), 0,
                "1000\n");
}

// ===========================================================================
// ROW 4 — the spelling inside a class that IMPLEMENTS AN INTERFACE (§27.6:
// "by reference for class- or interface-typed receivers").
//
// RED on the base: 72 B per formation. GREEN with the fix.
// ===========================================================================
BEZEL_TEST(bare_instance_method_group_closure_release_9216,
           interface_implementing_class_bare_instance_group_no_leak_spec_28_9) {
    const std::string src =
        std::string("module M;\n"
                    "public interface IA\n"
                    "  method Get(): int;\n"
                    "end IA;\n"
                    "public class P: IA\n") +
        kUnascribedLocal +
        std::string("  public static method Main(): int\n") + loopBody(200) +
        "  end Main;\n"
        "end P;\n";
    expectClean(runAotChecked(src), 0, "1000\n");
}

// ===========================================================================
// ROW 5 — the issue's OWN 300-iteration loop shape (`var f := Get;` inside a
// loop body), the measurement the issue reports (21 600 B / 600 allocations
// on this tree; 43 200 B / 1 200 allocations on the reporter's).
//
// RED on the base: 72 B per iteration. GREEN with the fix.
// ===========================================================================
BEZEL_TEST(bare_instance_method_group_closure_release_9216,
           issue_repro_300_iteration_loop_no_leak_spec_28_9) {
    const std::string src =
        "module M;\n"
        "public class P\n"
        "  private v: int;\n"
        "  public constructor(x: int)\n"
        "    this.v := x;\n"
        "  end constructor;\n"
        "  public method Get(): int => v;\n"
        "  public method Make(): int\n"
        "    var t := 0;\n"
        "    for var i in 0 ..< 300 do\n"
        "      var f := Get;\n"
        "      t := t + f();\n"
        "    end for;\n"
        "    return t;\n"
        "  end Make;\n"
        "  public static method Main(): int\n"
        "    var p := new P(3);\n"
        "    Console.WriteLine(p.Make());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    expectClean(runAotChecked(src), 0, "900\n");
}

// ── the two BASE-GREEN boundary controls (owner, 2026-11-12: at most two per
// suite, each named in the commit body with the wrong fix it catches) ──
//
// CONTROL 1 — the QUALIFIED twin of ROW 1 (`this.Get` instead of the bare
// `Get`). It was clean on the base. It catches the plausible wrong fix "arm
// the scope-end release for EVERY function-typed local, not only the producer
// ones": the qualified path ALREADY arms its release, so the local would
// release the one cell twice and the runtime's §28.9 over-release guard would
// fire ("refcount over-release (closure)").
BEZEL_TEST(bare_instance_method_group_closure_release_9216,
           qualified_twin_local_no_leak_spec_28_9) {
    expectClean(runAotChecked(program("", kQualifiedLocal, loopBody(200))), 0,
                "1000\n");
}

// CONTROL 2 — the BARE STATIC group of the enclosing class (the shape issue
// #9155 landed). It was clean on the base. It catches the plausible wrong fix
// "treat the INSTANCE group as the special case and narrow the producer arm
// to it" (`!isStaticMethod(ms)`): the static group's cell would then never
// be released and this row would leak 72 B per formation.
BEZEL_TEST(bare_instance_method_group_closure_release_9216,
           bare_static_group_no_leak_spec_28_9) {
    expectClean(runAotChecked(program("", kBareStatic, loopBody(200))), 0,
                "1000\n");
}
