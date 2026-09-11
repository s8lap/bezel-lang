// Codegen — issue #4234: a compile-time denial-of-service on wide/deep
// generic-record "diamonds" — declaring records where each level's fields
// all repeat the SAME nested record type (`Ri<T>` with `branch` fields all
// of type `R(i-1)<T>`), branch=3/depth=6, took 75.1s wall / 73.2s user CPU /
// 1.5GB RSS on a program that only DECLARES the types (zero uses, no
// `where T: new()`, no `default(T)`).
//
// MEASURED ROOT CAUSE (this run's own instrumentation, NOT the issue's own
// guess): the issue suspected `Resolver::recordValueContainsSelf` or
// `common::isBorrowBearingRecordCommon` (both declaration-time sema
// checks). Phase-by-phase timers proved `Resolver::run()`'s ENTIRE set of
// declaration-time checks — including both suspects — completes in under
// 1ms; `sema::resolveTypes`/`typecheckExpressions`/`checkOwnership` etc.
// were likewise instant. The cost was 100% inside `codegen::emitObjectFile`
// (specifically LLVM's own `pm.run(module)` codegen pass), because the
// emitted IR itself was exponentially large: one `bz$boxtostr$R4` function
// alone was 122,545 LLVM IR lines at branch=3/depth=4 (measured via
// BEZEL_DUMP_IR), growing ~3x per added level.
//
// The actual bug: `Emitter::emitValueToString`'s Record-kind arm
// (LlvmBackend_String.cpp) called `emitRecordToString(val, ts)` directly —
// plain C++ recursion with NO memoization — for every record-typed FIELD.
// A record type reached via `branch` sibling field positions therefore
// re-expanded its ENTIRE recursive field-listing body at every occurrence:
// O(branch^depth) LLVM instructions from a program that only declares the
// types. `Emitter::emitValueEquality` (LlvmBackend_Layout.cpp, calling
// `emitStructEquality`) and `Emitter::emitValueHashCode`
// (LlvmBackend_String.cpp, calling `emitRecordHashCode`) shared the
// IDENTICAL unmemoized-recursion shape — smaller per-node cost (an AND-
// chain / hash-fold vs a string-concat chain) but the same asymptotic
// growth: with ONLY the ToString arm fixed, an unfixed depth=8/branch=3
// diamond alone still cost 167s (measured), confirming the defect class
// reproduces one level of nesting later through the sibling arms.
//
// THE FIX (§9.1, v0.298): each of the three arms now routes its NESTED
// (field-level) recursion through a per-TypeSymbol MEMOIZED helper function
// (`recordToStringHelperFn` / `recordEqualityHelperFn` /
// `recordHashCodeHelperFn`, LlvmBackend_Setup.cpp) — a real LLVM `call`
// instead of re-inlining the recursive body. A record type is now built
// ONCE regardless of how many sibling positions reach it, collapsing the
// growth from O(branch^depth) to O(number of distinct record types
// declared). Measured post-fix: branch=3/depth=6 compiles in ~3s (down
// from 75-87s), and branch=3/depth=8 (which reproduced 167s with only the
// ToString arm fixed) also stays in the low single digits of seconds.
//
// §9.1: "ToString formatted as `"TypeName(Field1: value1, Field2: value2)"`."
// §8.5 "ToString": "Each component renders as its own `ToString` renders
// it (virtual dispatch …)" — the recursive clause this fix's memoization
// must preserve exactly, not just avoid re-expanding.
// §8.3 "Equals and GetHashCode": "Positional record | Field-by-field value
// equality … | Hash of all positional fields" and "`GetHashCode` must
// satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`."

#include <chrono>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Builds the exact "diamond" shape from the issue's own repro generator
// (gen_diamond_decl_only.py): `R0<T>(V: T)`, then `Ri<T>` with `branch`
// fields all of type `R(i-1)<T>`, for i = 1..depth. Declarations only — no
// instantiation, no `where T: new()`, no `default(T)` — matching the
// issue's finding that the cost is ENTIRELY in declaration processing.
std::string makeDiamondDecl(int branch, int depth, const char* moduleName) {
    std::string src = std::string("module ") + moduleName + "; ";
    src += "public record R0<T>(V: T); ";
    for (int i = 1; i <= depth; ++i) {
        src += "public record R" + std::to_string(i) + "<T>(";
        for (int j = 0; j < branch; ++j) {
            if (j != 0) src += ", ";
            src += "F" + std::to_string(j) + ": R" + std::to_string(i - 1) +
                   "<T>";
        }
        src += "); ";
    }
    src += "public class ProgDPX "
           "public static method Main(): int return 0; end Main; "
           "end ProgDPX;";
    return src;
}

// (A) CONTROL — §9.1/§8.5 correctness: a small (non-exponential) diamond's
// ToString still recurses into the SAME field type from two sibling
// positions and renders each independently. Must be UNCHANGED by the fix
// (the helper function must produce byte-identical output to the old
// inline recursion).
const char* kNestedToString =
    "module M4234A; "
    "public record Inner(V: int); "
    "public record Outer(A: Inner, B: Inner); "
    "public class C "
    "  public static method Main(): int "
    "    var o := new Outer(new Inner(1), new Inner(2)); "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (B) CONTROL — §8.3/§8.4 correctness: nested-record `=` (routed through
// emitValueEquality -> emitStructEquality, now via recordEqualityHelperFn)
// must still distinguish equal from unequal nested field values.
const char* kNestedEquals =
    "module M4234B; "
    "public record Inner(V: int); "
    "public record Outer(A: Inner, B: Inner); "
    "public class C "
    "  public static method Main(): int "
    "    var o1 := new Outer(new Inner(1), new Inner(2)); "
    "    var o2 := new Outer(new Inner(1), new Inner(2)); "
    "    var o3 := new Outer(new Inner(1), new Inner(3)); "
    "    Console.WriteLine((o1 = o2).ToString()); "
    "    Console.WriteLine((o1 = o3).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (C) CONTROL — §8.3's hash-equality invariant ("if a.Equals(b) then
// a.GetHashCode() = b.GetHashCode()") over a nested record, routed through
// emitValueHashCode -> emitRecordHashCode -> recordHashCodeHelperFn.
const char* kNestedHashConsistency =
    "module M4234C; "
    "public record Inner(V: int); "
    "public record Outer(A: Inner, B: Inner); "
    "public class C "
    "  public static method Main(): int "
    "    var o1 := new Outer(new Inner(1), new Inner(2)); "
    "    var o2 := new Outer(new Inner(1), new Inner(2)); "
    "    Console.WriteLine((o1.GetHashCode() = o2.GetHashCode()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// (A) CONTROL: nested-record ToString field listing is unaffected by the fix.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_record_tostring_field_listing_9_1_8_5) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedToString), "Outer(A: Inner(V: 1), B: Inner(V: 2))");
}

// ===========================================================================
// (B) CONTROL: nested-record `=` still distinguishes equal / unequal values.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_record_equals_field_by_field_8_3) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedEquals), "True\nFalse");
}

// ===========================================================================
// (C) CONTROL: the §8.3 hash-equality invariant holds over a nested record.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_record_hash_equality_invariant_8_3) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedHashConsistency), "True");
}

// ===========================================================================
// (D) THE PIN: branch=3/depth=6 declaration-only diamond (the issue's own
// measured repro shape) must compile+run within a bounded wall-clock time.
//
// Oracle choice: a VISIT/instruction-count assertion would be the more
// deterministic shape (LANE_RULES), but `runAot` is a black-box harness
// (fork-isolated `Driver::compileFile` + native link + exec) that does not
// expose an IR-level counter; reaching one would mean hand-rolling a
// separate lex/parse/sema/emitModule pipeline outside `runAot`, which is
// exactly the class of bespoke, lower-fidelity oracle the deprecated-JIT-
// harness guidance warns against (it would skip the ownership pass,
// definite-assignment, and verifyModule that runAot's real AOT path runs).
// A bounded wall-clock measured by the TEST around the real `runAot` call
// is therefore the correct oracle here, despite the shared-box-contention
// flakiness risk LANE_RULES flags for wall-clock assertions generally.
//
// Threshold: measured post-fix time for this exact shape is ~3s (this
// machine, unloaded); measured pre-fix time is 75-87s (this issue's own
// number, reproduced on this branch's parent). 20s is >6x the measured
// post-fix time (generous headroom for contention) and <1/3 of the
// measured pre-fix time (no risk of a real regression slipping under it).
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           branch3_depth6_diamond_declaration_only_compiles_under_20s_4234) {
    std::string src = makeDiamondDecl(/*branch=*/3, /*depth=*/6, "M4234DiamondPerf");
    auto t0 = std::chrono::steady_clock::now();
    JitResult r = runAot(src);
    auto t1 = std::chrono::steady_clock::now();
    double elapsedS = std::chrono::duration<double>(t1 - t0).count();
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    if (elapsedS >= 20.0) {
        std::cout << "    #4234 PERF REGRESSION: compiled in " << elapsedS
                  << "s (threshold 20s; pre-fix measured 75-87s)\n";
    }
    BEZEL_EXPECT_TRUE(elapsedS < 20.0);
}
