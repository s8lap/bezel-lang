// Regression test for issue #9187 — an indexer (getter) overloaded ONLY by a
// FUNCTION-TYPED parameter always bound the FIRST declared overload, on class-
// and interface-typed receivers alike; with five such overloads the program
// CRASHED on an ABI mismatch.
//
// THE DEFECT. §23.4 (v0.359): "Indexers may be overloaded by parameter type
// list." §27.1 (v0.359) makes a function type a first-class type, so an
// indexer's parameter may be function-typed (`property [f: () -> int]: int`),
// and §23.5 (v0.359) forbids only `ref`/`out` on an indexer parameter list.
//
// The SEMA side is the indexer overload matcher
// (src/sema/ExprTypecheck_Exprs.cpp's `allParamsMatch`): it compares a
// candidate's index-parameter type with the argument's type, and it answered
// `false` for every kind it did not spell out — a `TypeRef::Kind::Function`
// parameter among them. A function-typed index parameter therefore NEVER
// matched its argument, and the selection fell through to the `arityOnly`
// fallback — the FIRST arity-matching candidate.
//
// The CODEGEN side is the mirror, `Emitter::selectIndexer`
// (src/codegen/LlvmBackend_Assign.cpp): it compared the parameter with the
// argument through `indexerArgMatches` → `indexerArgMatchesUnder`, which
// answers `false` for a function kind too, so it ALSO fell back to the first
// arity-matching candidate. Both sides agreed on the wrong overload, so the
// program ran the WRONG overload's body.
//
// MEASURED on r30-c1 @ 169718adf (`scripts/agent_check.sh --probe --asan`):
//
//   * `c[lambda() => 7]` over `() -> string` / `() -> int` printed 1 — the
//     FIRST declared overload — where the expected value is 2;
//   * the five-overload shape printed 1, 1, 1 and then CRASHED:
//
//       ==1764780==ERROR: AddressSanitizer: SEGV on unknown address
//       0x000000000000 (pc 0x00000037a65f ...)
//           #0 0x00000037a65f in lam$3 bezel
//           #1 0x0000003773f0 in bz$indexer$get bezel
//
//     — the first-declared overload's body called the closure through a
//     signature that did not match the LAMBDA's own (the closure's real
//     return type is the lambda's inferred one, §27.3), so the call went
//     through a mismatched function pointer.
//
// THE CONTROL. The same shape declared as METHODS is correct: the method
// overload path prints 2 for `Pick(lambda() => 7)` and 1 / 15 / 107 / 1006 /
// 10000 for the five-overload shape (the METHOD twin, measured on the same
// base). §22.6 (v0.359)'s resolution pipeline ranks the APPLICABLE candidates
// by an EXACT match (step 4), and for a lambda-literal argument §22.6
// (*Lambda-literal arguments*) grades the position by the lambda's INFERRED
// RETURN TYPE: "The candidate is **exact** at the position when the inferred
// return type equals the return slot after type-argument substitution, and
// *converting* otherwise." §22.6 (*Parameter count at a function-typed
// position*) makes a candidate whose slot has a different parameter COUNT
// inapplicable, and §27.1 makes a function type's identity its arity, its
// parameter slots' modifiers and types, and its return type's ownedness and
// type.
//
// THE CHANGE. Two arms, one on each side, and they are the same predicate:
// sema's `Resolver::typeRefEqualSameScope` (`typeRefEqualImpl`'s
// `Kind::Function` arm) and codegen's `indexerParamTypeMatchesUnder` FUNCTION
// arm (#9137's, for the interface dispatch). A function type is compared SLOT
// BY SLOT, so a `() -> [3]int` parameter does not match a `() -> [4]int`
// argument (§15.1) and a `() -> int` one does not match a `() -> string`
// argument.
//
// THE ORACLE. Every row runs the program TWICE: once plain (`runAot`) for the
// VALUE oracle — the program's own exit code and output — and once checked
// (`runAotChecked`) for the MEMORY oracle. Each program loops 200 times, so a
// closure that leaked (§28.14: a function-typed parameter declared without
// `scoped` is CALL-BOUNDED — the callee keeps no reference to the callback
// after it returns, and the producer's +1 is the CALLER's to drop; §28.9: the
// closure object is runtime-managed, so its own reference must be dropped
// exactly once) is visible to the leak oracle.
//
// ROWS (all RED on r30-c1, all GREEN with the fix).
//   Row 1 — the CLASS-typed receiver: `c[lambda() => 7]`, 200 iterations.
//           RED on the base (200 — the first overload), GREEN with the fix
//           (400).
//   Row 2 — the INTERFACE-typed receiver: the same indexer read through an
//           interface-typed variable, 200 iterations. RED on the base (200),
//           GREEN with the fix (400).
//   Row 3 — FIVE overloads, the issue's own crashing shape (a `() -> ()`, a
//           `() -> int`, a `() -> [3]int`, a `() -> [4]int` and a `(int) ->
//           ()` parameter), each read 200 times. RED on the base (the
//           SIGSEGV), GREEN with the fix (2225800, no leak).
//   Row 4 — the SETTER twin: `fl[lambda() => 7] := 3;` through an
//           interface-typed variable, then a read, 200 iterations. RED on the
//           base (200 — the first overload's getter and setter), GREEN with
//           the fix (4400).
//
// The GREEN CONTROLS the issue names — the METHOD twin (which the METHOD path
// already answers correctly) and the `int`-parameter indexer twin — are
// green on the base and pin nothing, so they are NOT gtest rows: they are
// left with the corpus lane under
// /var/tmp/bezel-scratch/r43-m101-9187/probes/corpus-candidates/.
//
// per §11.2, §22.6, §23.4, §23.5, §27.1, §27.3, §28.9, §28.14, §15.1
// (issue #9187)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// The VALUE oracle: the program compiled, ran to completion, exited with its
// own exit code and printed exactly this output. The plain (uninstrumented)
// run has no sanitizer, so its exit code is the program's own.
void expectValue(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The MEMORY oracle: the checked run leaked NOTHING at all, and no memory
// error fired.
void expectNoLeak(const JitResult& r) {
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

// ── Row 1 (§23.4 / §27.1 / §27.3 / §22.6): the CLASS-typed receiver.
//
// The class declares two indexers whose parameter types differ only in the
// FUNCTION TYPE (`() -> string` and `() -> int`). The read's lambda infers
// the return type `int` (§27.3: an expression-bodied lambda's return type is
// its body's type), so §22.6's exact-match ranking names the `() -> int`
// overload — the SECOND one — and the read is 2, not the first overload's 1.
//
// 200 iterations: 200 * 2 = 400.
const char* kClassProgram = R"(
module Fn9187C;
public class FlFn9187C
  public property [f: () -> string]: int => 1;
  public property [f: () -> int]: int => 2;
  public constructor() end constructor;
end FlFn9187C;
public class ProgramFn9187C
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      using var c := new FlFn9187C();
      t := t + c[lambda() => 7];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end ProgramFn9187C;
)";

// ── Row 2 (§23.4 / §11.2 / §27.1 / §27.3 / §22.6): the INTERFACE-typed
// receiver — the same indexer read through an interface-typed variable, so
// the read dispatches through the itable.
//
// 200 iterations: 200 * 2 = 400.
const char* kInterfaceProgram = R"(
module Fn9187I;
public interface IFlFn9187I
  readonly property [f: () -> string]: int;
  readonly property [f: () -> int]: int;
end IFlFn9187I;
public class FlFn9187I : IFlFn9187I
  public property [f: () -> string]: int => 1;
  public property [f: () -> int]: int => 2;
  public constructor() end constructor;
end FlFn9187I;
public class ProgramFn9187I
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      using var c := new FlFn9187I();
      var fl: IFlFn9187I := c;
      t := t + fl[lambda() => 7];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end ProgramFn9187I;
)";

// ── Row 3 (§23.4 / §27.1 / §15.1 / §22.6): FIVE overloads — the issue's own
// crashing shape.
//
// The five parameter types are `() -> ()`, `() -> int`, `() -> [3]int`,
// `() -> [4]int` and `(int) -> ()`. The bodies CALL their parameter, so the
// first-declared overload's call to the closure went through a signature that
// did not match the lambda's own — the SIGSEGV the issue reports. The
// inline-array returns are distinct types (§15.1: the size is part of the
// type), so `() -> [3]int` and `() -> [4]int` are different overloads.
//
// Per iteration: 1 + (10 + 5) + (100 + 7) + (1000 + 6) + 10000 = 11129.
// 200 iterations: 200 * 11129 = 2225800.
const char* kFiveOverloadProgram = R"(
module Fn9187F;
public interface IFlFn9187F
  readonly property [q: () -> ()]: int;
  readonly property [q: () -> int]: int;
  readonly property [q: () -> [3]int]: int;
  readonly property [q: () -> [4]int]: int;
  readonly property [q: (int) -> ()]: int;
end IFlFn9187F;
public class FlFn9187F : IFlFn9187F
  public property [q: () -> ()]: int get => do q(); return 1; end; end;
  public property [q: () -> int]: int get => do return 10 + q(); end; end;
  public property [q: () -> [3]int]: int get => do var a := q(); return 100 + a[0]; end; end;
  public property [q: () -> [4]int]: int get => do var a := q(); return 1000 + a[3]; end; end;
  public property [q: (int) -> ()]: int get => do q(5); return 10000; end; end;
  public constructor() end constructor;
end FlFn9187F;
public class ProgramFn9187F
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      using var c := new FlFn9187F();
      var fl: IFlFn9187F := c;
      t := t + fl[lambda() => do end];
      t := t + fl[lambda() => 5];
      t := t + fl[lambda() => do var a: [3]int := [7, 8, 9]; return a; end];
      t := t + fl[lambda() => do var a: [4]int := [7, 8, 9, 6]; return a; end];
      t := t + fl[lambda(x: int) => do end];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end ProgramFn9187F;
)";

// ── Row 4 (§23.4 / §23.3 / §28.14): the SETTER twin.
//
// The class declares the two overloads with a getter AND a setter, and the
// setter stores a DIFFERENT value per overload — so the base's first-declared
// overload is visible in the READ that follows the write.
//
//   * the `() -> string` overload: the getter answers 1, the setter stores
//     `value + 97`;
//   * the `() -> int` overload: the getter answers `2 + _last`, the setter
//     stores `value + 17`.
//
// Fixed: the write goes to the `() -> int` overload's setter (`3 + 17 = 20`)
// and the read to its getter (`2 + 20 = 22`) — 22 per iteration, 200 * 22 =
// 4400. Base: the write goes to the FIRST overload's setter (`3 + 97 = 100`)
// and the read to its getter (1) — 200.
const char* kSetterProgram = R"(
module Fn9187S;
public interface IFlFn9187S
  property [f: () -> string]: int;
  property [f: () -> int]: int;
end IFlFn9187S;
public class FlFn9187S : IFlFn9187S
  private _last: int := 0;
  public property [f: () -> string]: int
    get => 1;
    set => do _last := value + 97; end;
  end;
  public property [f: () -> int]: int
    get => 2 + _last;
    set => do _last := value + 17; end;
  end;
  public constructor() end constructor;
end FlFn9187S;
public class ProgramFn9187S
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      using var c := new FlFn9187S();
      var fl: IFlFn9187S := c;
      fl[lambda() => 7] := 3;
      t := t + fl[lambda() => 7];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end ProgramFn9187S;
)";

BEZEL_TEST(indexer_fn_param_overload_resolution_9187,
           class_typed_read_of_a_function_typed_indexer_binds_the_matching_overload) {
    auto v = runAot(kClassProgram);
    expectValue(v, 0, "400\n");
    auto r = runAotChecked(kClassProgram);
    expectNoLeak(r);
}

BEZEL_TEST(indexer_fn_param_overload_resolution_9187,
           interface_typed_read_of_a_function_typed_indexer_binds_the_matching_overload) {
    auto v = runAot(kInterfaceProgram);
    expectValue(v, 0, "400\n");
    auto r = runAotChecked(kInterfaceProgram);
    expectNoLeak(r);
}

BEZEL_TEST(indexer_fn_param_overload_resolution_9187,
           five_function_typed_overloads_bind_each_its_own_overload) {
    auto v = runAot(kFiveOverloadProgram);
    expectValue(v, 0, "2225800\n");
    auto r = runAotChecked(kFiveOverloadProgram);
    expectNoLeak(r);
}

BEZEL_TEST(indexer_fn_param_overload_resolution_9187,
           interface_typed_write_of_a_function_typed_indexer_binds_the_matching_overload) {
    auto v = runAot(kSetterProgram);
    expectValue(v, 0, "4400\n");
    auto r = runAotChecked(kSetterProgram);
    expectNoLeak(r);
}

}  // namespace
