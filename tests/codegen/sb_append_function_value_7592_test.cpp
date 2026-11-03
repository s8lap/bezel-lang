// Regression tests for #7592 -- `sb.Append(g)` with a FUNCTION VALUE appended
// nothing and corrupted the heap (SEGV in `bz_string_release` from
// `bz_array_free`).
//
// THE DEFECT.
//
//   stdlib §3.1 (v0.353) declares, verbatim:
//
//     "public method Append(value: Object): StringBuilder;"
//
//   and its Contracts clause (stdlib §3.1, v0.353): "`Append(value: Object)`
//   keeps `value.ToString()`'s text, not `value`".
//
//   §8.1 (v0.353) makes a function type reach `Object` "via [its]
//   reference-type heap representation" -- so a function value at an `Object`
//   position is the OBJECT VIEW of its closure cell (the header the `Object`
//   slot holds), NOT the cell base. The `Object`-position machinery
//   (`fnAtObject_`, the #5978 change) records the sites where an operand "sits
//   where an `Object` is expected"; its table is keyed on the RECEIVER's own
//   TYPE ARGUMENT, so it covers the GENERIC collection heads
//   (`Array<T>.Add(item: T)` at an `Array<Object>`) and nothing else.
//
//   A NON-generic built-in with an `Object` parameter -- `StringBuilder`'s
//   `Append` -- was therefore NOT covered: the function value stayed the raw
//   closure CELL, whose word 0 is a CODE pointer. The StringBuilder's `Append`
//   lowering then took that cell for a STRING: `lowerObjectToString` asked
//   `bz_obj_is_string(recv, table)`, whose rule is "not a listed vtable => a
//   string", and a closure cell's word 0 (a code pointer) is not a listed
//   vtable => the cell was answered "a string". The builder's parts array --
//   an `Array<string>` -- therefore held the closure CELL as a part, and the
//   builder's own disposal released it as a string: `bz_string_release` read
//   the cell's word 3 as a refcount and freed a block that is not a string.
//
// MEASURED on the base tree (this lane's own worktree base `c3c33264`, the
// issue's own repro, `scripts/agent_check.sh --probe --asan`):
//
//     ==================================================================
//     ==NNNN==ERROR: AddressSanitizer: SEGV on unknown address ...
//     The signal is caused by a WRITE memory access.
//         #2 ... in bz_string_release
//         #4 ... in bz_array_free
//     SUMMARY: AddressSanitizer: SEGV on unknown address ...
//
//   and the builder's content was the cell's bytes read as text -- the append
//   "appended nothing" (the issue's title).
//
// §8.5 (v0.353) fixes the VALUE the append must produce: "For **function
// values** ... the stdlib §2.13 `FullName` rendering of the value's own
// function type -- the type it was **formed** at ... -- `"(int) -> int"` ...
// a lambda and a method reference render alike, **through `Object`** and at an
// interpolation hole alike." §12.7's *Function-typed targets* paragraph is
// where the formed type is fixed ("A function value's own type is the type it
// was formed at"). So `sb.Append(g)` must append the same text that
// `g.ToString()` prints -- "(int) -> int" -- and the two routes MUST agree.
//
// ── THE ORACLE ─────────────────────────────────────────────────────────────
// #7592's oracle is the CRASH and the RENDERED TEXT, NOT a leak count: the
// defect is a WRITE through a bad pointer, and the harness observes it twice
// over -- the program's exit code (the pre-fix program dies on the signal, so
// `r.ran` is false and `r.exitCode` is -1) and the sanitizer report itself
// (the pre-fix run carries "ERROR: AddressSanitizer: SEGV ... WRITE memory
// access" in the diagnostic the harness raises). Each row below asserts the
// POSITIVE: the program ran to completion, exited 0, printed the exact text,
// and NO AddressSanitizer report is present.
//
// WHY THESE ROWS USE THE LENIENT HARNESS (`runAotCheckedLenient`) -- AND IT
// IS NOT A WEAKENED ORACLE. The lenient harness turns LSan OFF and leaves the
// generated-code ASan ON, so the SEGV -- a WRITE memory access, exactly what
// ASan reports -- is fully observed. LSan is off because the program's
// FUNCTION VALUE carries a SEPARATE, PRE-EXISTING leak that is not #7592's:
// the closure cell behind a function value (64 bytes, `calloc`ed in `main`) is
// never reclaimed at exit, although §28.9 (v0.353) assigns the function
// value's lifetime to the runtime ("Runtime management governs the *function
// value itself* -- the heap-allocated closure object that backs it"). MEASURED
// on the BASE tree, through the `g.ToString()` route that this fix does not
// touch at all: "SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1
// allocation(s)". It is a residual of its own (reported, NOT claimed here);
// arming the leak oracle on it would measure THAT defect instead of #7592's.
// The rows that do not involve a function value use the strict harness and DO
// assert a zero leak count.
//
// ── THE CONTROL ROWS ───────────────────────────────────────────────────────
// Rows 4-7 are CONTROLS: they pass on the PRE-FIX tree as well, because
// #7592's defect is specific to a FUNCTION VALUE at an `Object` position. They
// are here to catch a fix that broke the neighbouring shapes -- a plain
// string, a class instance, an int, and the `AppendFormat` route -- and each
// says what it is a control FOR. They are NOT the regression oracle.
//
// Spec: §8.5, §8.1, §12.7 (*Function-typed targets*), §28.9, stdlib §3.1
// (v0.353).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Every diagnostic the harness produced, concatenated -- the sanitizer report
// the AOT harness embeds in the diagnostic message when the sanitizer fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// #7592's ORACLE: the program ran to completion, exited 0, printed the exact
// text, and NO AddressSanitizer report is present.
//
// The pre-fix program dies on the signal, so `r.ran` is false and
// `r.exitCode` is -1 (the harness's "sanitizer fired (crash/abort)" path),
// and its diagnostic carries the "ERROR: AddressSanitizer: SEGV ... WRITE
// memory access" report -- so this helper fails the row on the pre-fix tree
// twice over: on the exit code AND on the report's absence.
void expectNoMemoryError(const JitResult& r, const char* out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}

// The STRICT oracle (the generated-code ASan + LSan net): the program ran, it
// printed the expected text, and LSan reports ZERO leaked allocations and
// ZERO leaked bytes.
//
// Used by the CONTROL rows, whose programs own no function value and so leak
// nothing on either arm.
void expectNoLeak(const JitResult& r, const char* out) {
    expectNoMemoryError(r, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured.
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    // No sanitizer report of any kind: a double free would land here.
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

}  // namespace

// ===========================================================================
// 1/7 -- THE ISSUE'S OWN REPRO, verbatim.
//
//   var g: (int)->int := lambda(x: int) => x;
//   var sb := new StringBuilder();
//   sb.Append(g);
//
// MEASURED on the base tree BEFORE the fix: "AddressSanitizer: SEGV on
//   unknown address ... The signal is caused by a WRITE memory access ...
//   #2 bz_string_release ... #4 bz_array_free", and the builder's content was
//   the closure cell's bytes read as text (the append "appended nothing").
// AFTER the fix: the program exits 0 and the builder's content is the
//   function value's FORMED TYPE -- "(int) -> int" (§8.5).
// ===========================================================================
BEZEL_TEST(sb_append_function_value_7592,
           append_a_function_value_appends_its_formed_type) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class P
  public static method Main(): int
    var g: (int)->int := lambda(x: int) => x;
    var sb := new StringBuilder();
    sb.Append(g);
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "(int) -> int\n");
}

// ===========================================================================
// 2/7 -- THE BUILDER'S LENGTH: the appended TEXT is TWELVE CODE POINTS.
//
// `sb.Length` is the sum of the parts' code-point lengths (stdlib §3.1), so a
// row that pins the length pins that the part is the TEXT "(int) -> int" --
// TWELVE code points -- and not some other twelve-character string.
//
// MEASURED on the base tree BEFORE the fix: the crash.
// AFTER the fix: "12" and "(int) -> int".
// ===========================================================================
BEZEL_TEST(sb_append_function_value_7592,
           append_a_function_value_appends_twelve_code_points) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class P
  public static method Main(): int
    var g: (int)->int := lambda(x: int) => x;
    var sb := new StringBuilder();
    sb.Append(g);
    Console.WriteLine(sb.Length.ToString());
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "12\n(int) -> int\n");
}

// ===========================================================================
// 3/7 -- THE TWO ROUTES OF ONE RENDERING AGREE.
//
// stdlib §3.1: "`Append(value: Object)` keeps `value.ToString()`'s text". So
// the builder's content after `sb.Append(g)` and the text `g.ToString()`
// prints are ONE operation's two spellings and MUST be the same string.
//
// This is the row that makes the fix's SHAPE checkable rather than merely
// crash-free: a fix that rendered the function value by some OTHER spelling
// would have to reproduce "(int) -> int" twice, by two different rules.
//
// MEASURED on the base tree BEFORE the fix: the crash (the builder's content
//   was the cell's bytes; the `g.ToString()` route alone printed
//   "(int) -> int" -- the two routes DISAGREED).
// AFTER the fix: both print "(int) -> int".
// ===========================================================================
BEZEL_TEST(sb_append_function_value_7592,
           the_two_routes_of_a_function_value_rendering_agree) {
    expectNoMemoryError(runAotCheckedLenient(R"BZL(
module M;
class P
  public static method Main(): int
    var g: (int)->int := lambda(x: int) => x;
    Console.WriteLine(g.ToString());
    var sb := new StringBuilder();
    sb.Append(g);
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                        "(int) -> int\n(int) -> int\n");
}

// ===========================================================================
// 4/7 -- CONTROL: a PLAIN STRING still appends.
//
// WHAT IT IS A CONTROL FOR: #7592's fix converts the `Append(value: Object)`
// ARGUMENT at an `Object` position. A plain `string` is not a function value,
// so the conversion is a no-op for it -- but a fix that converted the
// argument UNCONDITIONALLY (or that disturbed the builder's part handling)
// would show up here.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control, not the
// regression oracle.
//
// MEASURED on both arms: "3" and "abc", no leak.
// ===========================================================================
BEZEL_TEST(sb_append_function_value_7592,
           append_a_plain_string_still_appends) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var sb := new StringBuilder();
    sb.Append("abc");
    Console.WriteLine(sb.Length.ToString());
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                 "3\nabc\n");
}

// ===========================================================================
// 5/7 -- CONTROL: a CLASS INSTANCE still appends.
//
// WHAT IT IS A CONTROL FOR: the class instance is the OTHER representation an
// `Object` slot holds -- it carries a VTABLE at word 0, so `Append` must
// still dispatch slot 3 (ToString) on it. A fix that made the argument an
// object VIEW unconditionally would have broken this row.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control, not the
// regression oracle.
//
// MEASURED on both arms: "1" and "W", no leak.
// ===========================================================================
BEZEL_TEST(sb_append_function_value_7592,
           append_a_class_instance_still_appends) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class W public override method ToString(): string => "W"; end W;
class P
  public static method Main(): int
    using var w := new W();
    var sb := new StringBuilder();
    sb.Append(w);
    Console.WriteLine(sb.Length.ToString());
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                 "1\nW\n");
}

// ===========================================================================
// 6/7 -- CONTROL: an INT still appends.
//
// WHAT IT IS A CONTROL FOR: an `int` reaches `Append` through the PRIMITIVE
// overload (`Append(value: int)`, stdlib §3.1), not through the `Object` one.
// A fix that rewired the argument's rendering wholesale would have shown up
// here.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control, not the
// regression oracle.
//
// MEASURED on both arms: "2" and "42", no leak.
// ===========================================================================
BEZEL_TEST(sb_append_function_value_7592,
           append_an_int_still_appends) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var sb := new StringBuilder();
    sb.Append(42);
    Console.WriteLine(sb.Length.ToString());
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                 "2\n42\n");
}

// ===========================================================================
// 7/7 -- CONTROL: the `AppendFormat` route is unaffected.
//
// WHAT IT IS A CONTROL FOR: `AppendFormat(format: string, args:
// Array<Object>)` (stdlib §3.1) is the OTHER `Object`-position consumer of
// the string surface, and its `args` elements reach the same
// `Object.ToString()` rendering. #7592's fix touches the `Append` ARGUMENT
// only; this row pins that the `AppendFormat` route still renders a `string`
// element as that string (the shape issue #7814 named, repaired elsewhere --
// this row is a REGRESSION GUARD for it, not a claim on it).
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control, not the
// regression oracle.
//
// MEASURED on both arms: "4" and "abcv", no leak.
// ===========================================================================
BEZEL_TEST(sb_append_function_value_7592,
           append_format_with_a_string_element_still_renders) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    using var a: Array<Object> := new Array<Object>();
    a.Add("v");
    var sb := new StringBuilder();
    sb.Append("abc");
    sb.AppendFormat("{0}", a);
    Console.WriteLine(sb.Length.ToString());
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end P;
)BZL"),
                 "4\nabcv\n");
}
