// Regression test for issue #9337 — an OWNED chain-call result
// (`Make(): () -> owned W`, §28.3.5's announcement table row 1) as the
// right-hand side of a `??=` whose TARGET DOES NOT OWN what it holds emitted
// MALFORMED IR: the chain-call arm's `ccres.slot` alloca was created inside the
// `coaleq.do` branch while the scope-end disposal it registers is emitted at the
// end of the ENCLOSING scope, which the alloca does not dominate.
//
//   Instruction does not dominate all uses!
//     %ccres.slot = alloca ptr, align 8
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// Positions (the issue's two programs, and the accessor twins):
//   1. a plain `ref` PARAMETER target (the callee does not own what it stores);
//   2. a NON-NULLABLE `for` element binding (§7.7: "the right operand is never
//      evaluated"; codegen still lowered it inside the branch);
//   3. a chain call NESTED in the right-hand side of a `??=` into an
//      auto-property / an indexer slot (the accessor path hands the RHS chain
//      call off, but a nested one keeps its own §28.3.11 registration).
//
// THE SPEC. §7.7: "The compound form `??=` evaluates its right-hand side, and
// invokes the setter, only when the assignment occurs"; §28.3.11 *Unbound owned
// temporaries* (v0.359): an owned temporary is disposed at the end of the full
// expression in which it is produced. The right operand runs on only SOME paths,
// so the slot is an entry-block null-initialised alloca (issue #9270's
// discipline): a path that skipped the operand reaches the null-guarded
// disposal with null, and the value is disposed exactly once where it ran.
//
// THE ORACLE. `runAotChecked`: AddressSanitizer over the GENERATED module and
// LeakSanitizer at exit; every program COUNTS its destructor runs. Rows LOOP
// (200 iterations).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// The class the chain call mints; it COUNTS its destructor runs.
const char* kCountingClass =
    "public class W "
    "  public static Drops: int := 0; "
    "  public V: int := 3; "
    "  destructor "
    "    W.Drops := W.Drops + 1; "
    "  end destructor; "
    "end W; ";

// A FUNCTION VALUE whose declared return carries `owned`: §28.3.5's table
// (row 1) makes the invocation's result OWNED.
const char* kProducer =
    "public static method Make(): () -> owned W "
    "  return lambda(): W => new W(); "
    "end Make; ";

// `Run` holds the construct so every owner has been disposed by the time
// `Main` reads the destructor count.
std::string program(const std::string& members, const std::string& runBody,
                    const std::string& extraClasses = "") {
    return std::string("module M; ") + kCountingClass + extraClasses +
        "public class C "
        "  " + kProducer + members +
        "  public static method Run(): int "
        "    var t := 0; " + runBody +
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ROW 1 — the issue's position 1: `w ??= C.Make()();` into a `ref` PARAMETER,
// target null. The callee does not own the target, so the value is an unbound
// temporary of the `??=` statement and is disposed once, at the end of `Fill`:
// 200 values, 200 destructor runs. (The caller never reads `w` afterwards; the
// `:=` twin of this store is the same disposal.)
//
// RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_assign_nonowning_9337,
           ref_parameter_target_chain_call_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Fill(ref w: W?): int "
        "  w ??= C.Make()(); "
        "  return 1; "
        "end Fill; "
        "public static method Go(): int "
        "  var w: W? := null; "
        "  return C.Fill(ref w); "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(); "
        "end; "));
    expectClean(r, 0, "200\n200\n");
}

// ROW 2 — the same `ref` parameter, target NON-null: the right operand is never
// evaluated, so nothing is produced and nothing is disposed except the
// caller's own `o` (200 constructions, 200 runs). RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_assign_nonowning_9337,
           ref_parameter_target_non_null_never_evaluates_the_chain_call) {
    JitResult r = runAotChecked(program(
        "public static method Fill(ref w: W?): int "
        "  w ??= C.Make()(); "
        "  return 1; "
        "end Fill; "
        "public static method Go(o: W): int "
        "  var w: W? := o; "
        "  return C.Fill(ref w); "
        "end Go; ",
        "for var j in 0..<200 do "
        "  using var o := new W(); "
        "  t := t + C.Go(o); "
        "end; "));
    expectClean(r, 0, "200\n200\n");
}

// ROW 3 — the issue's position 2: a NON-NULLABLE `for` element. §7.7 / the
// compiler's own warning: the right operand is never evaluated. 200 arrays of
// one element, each disposed with its owner: 200 runs, none from `Make`.
// RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_assign_nonowning_9337,
           non_nullable_for_element_never_evaluates_the_chain_call) {
    JitResult r = runAotChecked(program(
        "public static method Go(): int "
        "  var xs := new OwningArray<W>(); "
        "  xs.Add(new W()); "
        "  var n := 0; "
        "  for var w in xs do "
        "    w ??= C.Make()(); "
        "    n := n + w.V; "
        "  end; "
        "  return n; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(); "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 4 — a chain call NESTED in the right-hand side of a `??=` into an
// AUTO-PROPERTY (the accessor path): `C.Peek(C.Make()())` borrows the value for
// the call, and the value is an unbound temporary disposed at the end of the
// statement: 200 values, 200 runs. RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_assign_nonowning_9337,
           property_target_nested_chain_call_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Peek(w: W): int "
        "  return w.V; "
        "end Peek; "
        "public static method Go(): int "
        "  var h := new H(); "
        "  h.Count ??= C.Peek(C.Make()()); "
        "  return h.Count!; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(); "
        "end; ",
        "public class H "
        "  public property Count: int? := null; "
        "  public constructor() end constructor; "
        "end H; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 5 — the INDEXER twin of row 4.
BEZEL_TEST(owned_chain_call_coalesce_assign_nonowning_9337,
           indexer_target_nested_chain_call_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Peek(w: W): int "
        "  return w.V; "
        "end Peek; "
        "public static method Go(): int "
        "  var h := new H(); "
        "  h[1] ??= C.Peek(C.Make()()); "
        "  return h[1]!; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(); "
        "end; ",
        "public class H "
        "  private _v: int? := null; "
        "  public property [i: int]: int? "
        "    get => this._v; "
        "    set => do this._v := value; end; "
        "  end; "
        "  public constructor() end constructor; "
        "end H; "));
    expectClean(r, 0, "600\n200\n");
}
