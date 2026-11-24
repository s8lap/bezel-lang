// Codegen — issue #9005: a NESTED lambda that ASSIGNS the captured string
// PARAMETER of its enclosing method over-released the caller's string, and the
// caller's own scope-end release then freed it a second time.
//
// Spec: §27.5 (v0.359) "Closures" — "Captured variables are heap-allocated in
// a closure object. Modifications to a captured variable inside the lambda are
// visible outside, and vice versa." (the heap cell is SHARED by the enclosing
// frame and every closure env; that sharing is the implementation of this
// rule.); §28.9 (v0.359) "Runtime-managed types" (`string` and function values
// are refcounted: every co-owner takes exactly one reference and releases
// exactly one); §28.1 (v0.359) "Where the `owned` modifier can appear" — "On
// any other type — a primitive, `string`, a function type, an ordinary record,
// an enum, a non-owning inline array, or the unit type — `owned` is a compile
// error: such values carry no disposal obligation to transfer (§28.9)" — so a
// by-value `string` parameter is an unmodified BORROW whose single reference
// belongs to the CALLER. Spec revision v0.359.
//
// THE BUG. A method returns a nested lambda whose inner lambda ASSIGNS the
// method's own `string` parameter:
//
//     public static method Ident(p: string): () -> () -> string
//       return lambda() => lambda(): string => do p := p + "z"; return p; end;
//     end Ident;
//
// MEASURED on the base (r30-c1 @ b666febe8), `bezelc --sanitize=address`:
//
//     ==ERROR: AddressSanitizer: SEGV on unknown address 0xffffffffffffffff
//       #1 free
//       #2 bz_string_release build/../src/runtime/bezel_runtime_refcounting.cpp:168:5
//       #3 bz$P.Program$Run
//     SUMMARY: AddressSanitizer: SEGV ... in __asan::Allocator::Deallocate
//
// ROOT CAUSE. Three parties claim the reference of a capture cell's CONTENT:
// the per-capture retain (#2689, taken on the value present AT CAPTURE), every
// env's envdtor release (on the value present AT DESTRUCTION), and the WRITE's
// drop-on-overwrite release (lowerStringLocalReassign's `strl.old` release, on
// the value the write DISPLACES). #2706 suppresses the first TWO together for a
// cell that is WRITTEN and shared by more than one env (`mutatedSharedCaptureCell`:
// "Both sides are suppressed together … which leaks the cell's final value
// instead of over-releasing it"). The WRITE's release was not suppressed with
// them, and it is not sound there: a captured LOCAL's cell inherits the local's
// producer +1 (the escape walk suppresses the local's scope-end release), so
// the write consumes a reference the cell really owns — but a captured
// PARAMETER's cell owns NOTHING once the #2689 retain is suppressed (§28.1: the
// value in the cell is still the CALLER's), so the write dropped the caller's
// reference and the caller's scope-end release freed the same string again.
//
// THE FIX. The capture side now records, per capture-cell HOLDER, that the
// cell's content is not the cell's to release (`unownedCellContentSlots_`), and
// the write paths (lowerStringLocalReassign for `s := e`, the §4.6 `s += e`
// branch) skip the drop-on-overwrite release for such a cell. A frame that
// owns a reference of its own — #2696's bind-time retain for a `string`
// parameter this body assigns to — keeps its release: that reference is the one
// the write consumes.
//
// SUPERSEDED by #9025 / #9026 (v0.359): that skip leaked every displaced value
// (#9026), and left the cell's seed a pure borrow that an escaping closure
// could outlive (#9025). The cell now mints one reference to its seed when it
// is allocated (`cellSeedRetainSlots_`), and the write's release is no longer
// skipped. The rows below pin the same double free and stay unchanged; see
// lambda_captured_param_cell_9025_test.cpp.
//
// WHAT THE ROWS BELOW SEE. The cell's final value is then leaked rather than
// over-released (the same bounded-leak trade #2706 makes, and the same one
// #2718 records as the price of not refcounting the cell), so the rows run on
// `runAotCheckedLenient` — generated-code AddressSanitizer ON, LeakSanitizer
// OFF — and compare the full stdout: pre-fix every row is a crash (no output
// at all), post-fix each prints its expected total.
//
// ⚠ EVERY STRING IS BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL on which `bz_string_release` is a NO-OP, so a literal-built probe
// passes whether or not the bug is present. Row (B) therefore builds its
// string with an interpolation, and row (A) — the issue's literal repro — is
// kept verbatim because its `"hi" + "x"` is NOT folded (it crashes pre-fix,
// which is the row's whole point).

#include <string>

#include "support/aot_support.hpp"   // runAotCheckedLenient — ASan on, LSan off
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotCheckedLenient;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off); a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// =====================================================================
// (A) ISSUE #9005's REPRO, VERBATIM. The nested lambda ASSIGNS the
// captured string parameter; the method's caller owns the string and
// releases it at its scope end.
//
// The captured value starts as "hix" (3 characters), so the first inner
// call returns "hixz" (4) and the second one — reading the value the
// first call left in the shared cell — returns "hixzz" (5): 9 per call,
// 200 calls, 1800.
// =====================================================================
const char* kIssueRepro =
    "module P; "
    "public class Program "
    "  public static method Ident(p: string): () -> () -> string "
    "    return lambda() => lambda(): string => do p := p + \"z\"; return p; end; "
    "  end Ident; "
    "  public static method Run(): int "
    "    var s := \"hi\" + \"x\"; "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var f := Program.Ident(s); "
    "      t := t + f()().Length + f()().Length; "
    "    end; "
    "    return t; "
    "  end Run; "
    "  public static method Main(): int "
    "    Console.WriteLine(Program.Run()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (B) THE SAME SHAPE with the string built at RUNTIME (an interpolation
// of a single digit, so the length stays 3 and the total stays 1800).
// Guards the row against a future constant fold of `"hi" + "x"`, which
// would silently turn (A) into a vacuous row: an RC-SENTINEL is released
// by a no-op.
// =====================================================================
const char* kRuntimeBuiltString =
    "module P; "
    "public class Program "
    "  public static method Ident(p: string): () -> () -> string "
    "    return lambda() => lambda(): string => do p := p + \"z\"; return p; end; "
    "  end Ident; "
    "  public static method Run(): int "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var s := \"hi\" + $\"{j mod 10}\"; "
    "      var f := Program.Ident(s); "
    "      t := t + f()().Length + f()().Length; "
    "    end; "
    "    return t; "
    "  end Run; "
    "  public static method Main(): int "
    "    Console.WriteLine(Program.Run()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// =====================================================================
// (C) THE SAME DEFECT WITHOUT THE NESTING: two lambdas in the SAME
// frame capture the parameter, and the second one writes it. The cell is
// shared by two envs, so #2706 suppresses the capture retain and the
// envdtor release, and the write's release-old drops the caller's
// reference — the same over-release, one level shallower.
//
// The captured value starts as "hix" (3), the writing lambda returns
// "hixz" (4): 4 per call, 200 calls, 800.
// =====================================================================
const char* kTwoEnvsSameFrame =
    "module P; "
    "public class Program "
    "  public static method Ident(p: string): () -> string "
    "    var f1 := lambda(): string => do return p; end; "
    "    var f2 := lambda(): string => do p := p + \"z\"; return p; end; "
    "    return f2; "
    "  end Ident; "
    "  public static method Run(): int "
    "    var s := \"hi\" + \"x\"; "
    "    var t := 0; "
    "    for var j in 0..<200 do "
    "      var f := Program.Ident(s); "
    "      t := t + f().Length; "
    "    end; "
    "    return t; "
    "  end Run; "
    "  public static method Main(): int "
    "    Console.WriteLine(Program.Run()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

// (A) The issue's repro: pre-fix a SEGV in bz_string_release (no output at
// all), post-fix the expected total.
BEZEL_TEST(codegen_lambda_captured_param_cell_write_9005,
           issue_repro_nested_lambda_assigns_captured_param_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kIssueRepro), "1800\n");
}

// (B) The same shape with a runtime-built string.
BEZEL_TEST(codegen_lambda_captured_param_cell_write_9005,
           runtime_built_param_string_no_double_free_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kRuntimeBuiltString), "1800\n");
}

// (C) The same defect one level shallower (two envs in one frame).
BEZEL_TEST(codegen_lambda_captured_param_cell_write_9005,
           two_envs_one_frame_param_write_no_double_free_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(kTwoEnvsSameFrame), "800\n");
}
