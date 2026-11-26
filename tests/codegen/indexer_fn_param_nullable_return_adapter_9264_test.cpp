// Regression test for issue #9264 — an indexer whose INDEX PARAMETER is
// `() -> int?`, called with a lambda literal, read GARBAGE: the `int` → `int?`
// RETURN ADAPTER that the METHOD twin applies was missing on the index path.
//
// THE DEFECT. §5.1 (v0.359) *Nullable conversions*: "For any type `T`, `T`
// implicitly converts to `T?` … for a value type, the present-value wrapping
// (§7.2)". So a lambda literal whose body yields an `int` is ADMITTED at a
// `() -> int?` slot. §23.4 (v0.359) makes an indexer's parameter list a
// parameter list ("Indexers may be overloaded by parameter type list"), and
// §27.3 (v0.359) *Admission at a target-typed non-call position* says what
// the compiler owes the admitted lambda: "at a call argument the same check
// applies to the slot after §14.5's substitution — the body is typed, and its
// inferred return type must equal the target's return slot or implicitly
// convert to it (§5.1)". The body is TYPED AT THE SITE — the compiler types it
// against the slot and bridges the two, rather than calling the closure
// through an ABI it does not have.
//
// THE METHOD TWIN IS THE ORACLE, AND IT WAS ALREADY CORRECT. The METHOD path
// supplies the slot's function type to the lambda's own lowering TWICE: to
// SEMA, for the §27.3 CHECK (the matched formal's return slot is threaded
// into the lambda's visit), and to CODEGEN, for the ADAPTER —
// `buildOrderedArgs` (src/codegen/LlvmBackend_Assign.cpp) sets
// `expectedFunctionType_` to the matched formal's FUNCTION TYPE whenever the
// argument is a lambda literal, and `lowerLambda`
// (src/codegen/LlvmBackend_Exprs2.cpp) then types the CLOSURE'S RETURN at
// that slot: for a nullable value type the tagged `{ i8, i32 }` carrier (fix
// #543), with the body value wrapped into it.
//
// The INDEX path supplied the target to SEMA only — fix #9230 added the §27.3
// CHECK at an index position, so the ADMISSION was already right — and never
// to CODEGEN. So the closure kept its own inferred return type, and the
// accessor called it through the SLOT's ABI.
//
// MEASURED on r30-c1 @ ec6d77840 (the base), `scripts/agent_check.sh --probe
// --asan` on the issue's own program (`lambda() => 7`):
//
//   == the indexer prints -1234714617, not 7 ==
//
// — and the value DIFFERED RUN TO RUN (other runs of the same shape, on the
// same or an earlier base, printed -100941817, 999538695, -167788537,
// -481837049 and -2106605561): garbage, not a stable wrong answer. The METHOD
// twin `M(f: () -> int?)` called with the SAME lambda printed 7. ASan/LSan
// reported NOTHING: this is a wrong-VALUE miscompile (an ABI mismatch), not a
// memory error.
//
// IN THE IR, on the base:
//
//   define internal i32        @"lam$0"(ptr %0)     ; the indexer path
//   define internal { i8, i32 } @"lam$0"(ptr %0)   ; the METHOD twin
//
// — and with the fix the indexer path emits the SECOND shape: the closure
// returns the `int?` carrier, exactly as the method path's does.
//
// WHY THE ROWS USE 256 AND NOT THE ISSUE'S 7. On the base the accessor calls
// the closure through the `int?` ABI (`call { i8, i32 } %fn(ptr %env)`) while
// the closure returns a bare `i32`. The `{ i8, i32 }` carrier comes back in
// TWO registers (x86-64 SysV: the `hasValue` byte in the first, the payload in
// the second), and the callee fills only the first with its `int` return: so
// on the base the accessor's `hasValue` byte IS the low byte of the closure's
// return value, and its payload is whatever the second register happened to
// hold. With the issue's `7` the base's `hasValue` byte is 7 (nonzero ⇒ "not
// null") and the stale payload register happened to hold 7 in the AOT
// harness — so the base printed the CORRECT ANSWER by coincidence, and a row
// asserting "7" would be GREEN on the base (and would pin nothing). With 256
// the low byte is 0, so the base's accessor sees NULL and returns -1 — a
// DETERMINISTIC wrong answer — while the fix returns 256. The value 256 is
// not a multiple of 256 by accident: it is `0 mod 256`, which is what makes
// the base's mis-read deterministic.
//
// THE FIX. `indexerArgExpectedFuncTy` +
// `lowerIndexerGet`/`lowerIndexerSet`/`lowerIndexerCompoundCore`'s
// `lowerIndexArg` (src/codegen/LlvmBackend_Assign.cpp): every index-argument
// lowering site now threads the indexer's k-th index-parameter type into
// `lowerLambda` as `expectedFunctionType_` — the METHOD path's ONE adapter,
// reused, not a second one.
//
// ROWS (all RED on r30-c1 @ ec6d77840 — `0 passed, 4 failed` — and all GREEN
// with the fix — `4 passed, 0 failed`):
//   Row 1 — the GET accessor, the issue's own program: `c[lambda() => 256]`
//           at `this[f: () -> int?]` must print 256. RED on the base (the
//           accessor saw NULL and printed -1), GREEN with the fix.
//   Row 2 — the SET accessor twin: the setter calls the same slot, so the
//           same closure is called through the same ABI.
//   Row 3 — the INTERFACE-typed receiver: the same read through an
//           interface-typed variable, so the accessor dispatches through the
//           ITABLE — a different lowering path, the same slot.
//   Row 4 — 200 ITERATIONS of the issue's program, with a `runAotChecked`
//           leak leg: the value (51200 = 200 × 256) AND the memory oracle
//           (§28.9 / §28.14: the closure object is runtime-managed and its
//           reference must be dropped exactly once, so a leak is visible only
//           over many iterations).
//
// The green-on-base twins the issue names — the METHOD twin, the issue's
// literal `lambda() => 7` program (which the base answers correctly BY
// ACCIDENT, see above), and a matching non-nullable return at an index slot —
// are NOT gtest rows (a row that is green on the base pins nothing): they are
// left with the corpus lane under
// /var/tmp/bezel-scratch/r43-m137-9264/probes/corpus-candidates/ with their
// deciding spec sentences.
//
// per §5.1, §23.4, §27.3, §28.9, §28.14 (issue #9264)

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

// The VALUE oracle: the program ran, exited 0, and printed exactly `expected`.
void expectPrinted(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// ── Row 1 (§5.1 / §23.4 / §27.3): the GET accessor — the issue's own
// program.
//
// The accessor CALLS its parameter (`f()`), so on the base the closure is
// called through the slot's `int?` ABI while the closure returns a bare `int`
// — the garbage the issue reports, and (with the 256 body) the deterministic
// "-1" (the accessor saw NULL).
const char* kGetProgram = R"(
module Ix9264G;
public class Ix9264G
  public property this[f: () -> int?]: int
    get => do var s := f(); if s <> null then return s!; end if; return -1; end;
  end;
  public constructor() end constructor;
end Ix9264G;
public class ProgramIx9264G
  public static method Main(): int
    using var c := new Ix9264G();
    Console.WriteLine(c[lambda() => 256]);
    return 0;
  end Main;
end ProgramIx9264G;
)";

// ── Row 2 (§5.1 / §23.4 / §27.3): the SET accessor twin.
//
// The setter calls the same slot, so the same closure is called through the
// same ABI — on the base it sees NULL and stores -1; with the fix it stores
// the value 256.
const char* kSetProgram = R"(
module Ix9264S;
public class Ix9264S
  public property this[f: () -> int?]: int
    get => do var s := f(); if s <> null then return s!; end if; return -1; end;
    set => do var s := f(); if s <> null then _last := s!; else _last := -1; end if; end;
  end;
  private _last: int;
  public method Last(): int => _last;
  public constructor() end constructor;
end Ix9264S;
public class ProgramIx9264S
  public static method Main(): int
    using var c := new Ix9264S();
    c[lambda() => 256] := 3;
    Console.WriteLine(c.Last());
    return 0;
  end Main;
end ProgramIx9264S;
)";

// ── Row 3 (§5.1 / §11.2 / §23.4 / §27.3): the INTERFACE-typed receiver.
//
// The same read through an interface-typed variable, so the accessor
// dispatches through the ITABLE — a different lowering path, the same slot.
const char* kInterfaceProgram = R"(
module Ix9264I;
public interface IIx9264I
  readonly property [f: () -> int?]: int;
end IIx9264I;
public class Ix9264I : IIx9264I
  public property this[f: () -> int?]: int
    get => do var s := f(); if s <> null then return s!; end if; return -1; end;
  end;
  public constructor() end constructor;
end Ix9264I;
public class ProgramIx9264I
  public static method Main(): int
    using var c := new Ix9264I();
    var i: IIx9264I := c;
    Console.WriteLine(i[lambda() => 256]);
    return 0;
  end Main;
end ProgramIx9264I;
)";

// ── Row 4 (§5.1 / §23.4 / §27.3 / §28.9 / §28.14): 200 ITERATIONS of the
// issue's program.
//
// The 200 iterations are the leak oracle's window (§28.9 / §28.14: the
// closure object is runtime-managed and its reference must be dropped exactly
// once, so a leaked closure is visible only over many iterations) and they
// widen the value oracle: 200 × 256 = 51200 (the base: 200 × -1 = -200).
const char* kLoopProgram = R"(
module Ix9264L;
public class Ix9264L
  public property this[f: () -> int?]: int
    get => do var s := f(); if s <> null then return s!; end if; return -1; end;
  end;
  public constructor() end constructor;
end Ix9264L;
public class ProgramIx9264L
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      using var c := new Ix9264L();
      t := t + c[lambda() => 256];
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end ProgramIx9264L;
)";

BEZEL_TEST(indexer_fn_param_nullable_return_adapter_9264,
           get_accessor_returns_the_slots_value) {
    expectPrinted(runAot(kGetProgram), "256\n");
}

BEZEL_TEST(indexer_fn_param_nullable_return_adapter_9264,
           set_accessor_returns_the_slots_value) {
    expectPrinted(runAot(kSetProgram), "256\n");
}

BEZEL_TEST(indexer_fn_param_nullable_return_adapter_9264,
           interface_typed_receiver_returns_the_slots_value) {
    expectPrinted(runAot(kInterfaceProgram), "256\n");
}

BEZEL_TEST(indexer_fn_param_nullable_return_adapter_9264,
           two_hundred_iterations_return_the_slots_value_and_leak_nothing) {
    expectPrinted(runAot(kLoopProgram), "51200\n");
    expectNoLeak(runAotChecked(kLoopProgram));
}

}  // namespace
