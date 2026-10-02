// Codegen + Layout — zero-field record captured by a closure (§28.9, v0.313).
//
// A record with no fields still has identity and is still a first-class value
// type: `new E()` yields a value, and §28.9 makes it capturable by a closure
// (a bound method reference `e.Who`, or a lambda reading `e`). Capturing it
// routes the value through `Emitter::boxLocalSlot`, which allocates a heap
// CELL whose byte size is `ptrtoint (gep <valTy>, null, 1)` and then stores the
// value into it.
//
// THE BUG: `recordStruct` built the record's LLVM struct body from its storage
// symbols alone, so a record declaring no fields lowered to `type {}` — zero
// bytes. Both halves of the capture then degenerate: the `alloca` of the
// captured local is a zero-byte stack object, and the cell size expression
// evaluates to 0, so the cell is `calloc(1, 0)`, a zero-byte block. The store
// that initialises the cell lands outside any addressable byte. Under
// `bezelc --sanitize=address` this is reported and the program aborts
// (`AddressSanitizer:DEADLYSIGNAL`); WITHOUT a sanitizer the same programs run
// and print the right answers, which is why it went unnoticed.
//
// The fix gives a zero-field record a one-byte LLVM footprint (a single
// unnamed `i8` padding field, appended after the real fields so `fieldIndex_`
// is untouched). That makes the cell size 1 at all three capture sites by
// construction. Records that declare any field keep their exact previous
// layout, so nothing else moves.
//
// ORACLE. The unsanitized program is CORRECT both before and after the fix —
// it prints "E" and exits 0 either way — so stdout alone cannot pin this, and a
// plain `runAotOut` row would pass against the broken compiler.
//
// The discriminating oracle is the generated-code sanitizer sweep: under
// `BEZEL_AOT_FORCE_SANITIZE=address` (`forceSanitizeAddress()`, the same switch
// CLAUDE.md requires for any allocation/layout change) `runAotOut` itself
// compiles the program with `bezelc --sanitize=address`, and the unfixed
// compiler aborts it on the zero-byte store. MEASURED on this branch: with the
// fix reverted, rows 1 and 2 fail on the `runAotOut` assertion under that sweep
// while all four controls pass; with the fix they pass in both modes.
//
// `runUnderAsanExitCode` is asserted alongside as a second, independent leg. Note
// it links the ASan runtime but emits the generated object WITHOUT shadow checks
// (`lowerToObjectForAsan` calls `emitObjectFile` with `sanitizeAddress` off), so
// on its own it sees only runtime-library faults — it does not substitute for
// the sweep above, and -1 (runtime unavailable) is skipped rather than asserted.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/force_sanitize.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

using bezel::test::forceSanitizeAddress;
using bezel::test::runAotOut;
using bezel::test::runUnderAsanExitCode;

namespace {

// A zero-field record with an ordinary (non-extension) method.
constexpr const char* kZeroFieldRecord =
    "record E() "
    "  public method Who(): string return \"E\"; end Who; "
    "end E; ";

// Assert the program prints `expected`, and that it does so with no reported
// memory error. `tag` names the scratch stem so a failing run leaves its
// artifacts beside the others.
void expectClean(const std::string& source, const char* tag,
                 const char* expected) {
    // Under the sweep this IS the memory-safety assertion: runAotOut compiles
    // with --sanitize=address there, so an unfixed compiler makes the program
    // abort and the comparison fails. In the default mode it is the value pin.
    BEZEL_EXPECT_EQ(runAotOut(source), expected);

    if (!forceSanitizeAddress()) return;  // second leg is a sweep-only oracle

    int asan = runUnderAsanExitCode(source, tag);
    if (asan == -1) return;  // toolchain unavailable: ASan leg cannot run here
    BEZEL_EXPECT_EQ(asan, 0);
}

}  // namespace

// R1 (the pin): a bound method reference over a zero-field record. This is the
// guarded capture-cell site — the cell is allocated on the first pass and
// reused, then the method reference is stored into it.
BEZEL_TEST(zero_field_record_capture_cell_spec_28_9,
           spec_28_9_bound_method_ref_capture_of_zero_field_record) {
    expectClean(
        std::string("module M; ") + kZeroFieldRecord +
        "class C public static method Main() "
        "  var e := new E(); "
        "  var m: () -> string := e.Who; "
        "  Console.WriteLine(m()); "
        "end Main; end C;",
        "zerofield_1_method", "E");
}

// R2 (the pin): a lambda capturing a zero-field record by value. The same cell
// allocation, reached through the lambda-capture path.
BEZEL_TEST(zero_field_record_capture_cell_spec_28_9,
           spec_28_9_lambda_capture_of_zero_field_record) {
    expectClean(
        std::string("module M; ") + kZeroFieldRecord +
        "class C public static method Main() "
        "  var e := new E(); "
        "  var f: () -> string := lambda() => e.Who(); "
        "  Console.WriteLine(f()); "
        "end Main; end C;",
        "zerofield_2_lambda", "E");
}

// Control A: a DIRECT call builds no closure and no cell, so it must be clean
// both before and after the fix — it isolates the capture as the trigger.
BEZEL_TEST(zero_field_record_capture_cell_spec_28_9,
           spec_28_9_direct_call_control_no_closure_env) {
    expectClean(
        std::string("module M; ") + kZeroFieldRecord +
        "class C public static method Main() "
        "  var e := new E(); "
        "  Console.WriteLine(e.Who()); "
        "end Main; end C;",
        "zerofield_3_direct", "E");
}

// Control B: binding a COPY and calling it directly also builds no closure.
// Together with Control A this shows the record VALUE itself was never the
// problem — only boxing it into a capture cell was.
BEZEL_TEST(zero_field_record_capture_cell_spec_28_9,
           spec_28_9_copy_then_direct_call_control_no_closure_env) {
    expectClean(
        std::string("module M; ") + kZeroFieldRecord +
        "class C public static method Main() "
        "  var e := new E(); "
        "  var f := e; "
        "  Console.WriteLine(f.Who()); "
        "end Main; end C;",
        "zerofield_4_copy", "E");
}

// Control C: a record that DOES declare a field is untouched by the fix — its
// layout, capture and result must be exactly what they were.
BEZEL_TEST(zero_field_record_capture_cell_spec_28_9,
           spec_28_9_capture_of_non_empty_record_layout_unchanged) {
    expectClean(
        "module M; "
        "record R(A: int) "
        "  public method Show(): string return $\"R{A}\"; end Show; "
        "end R; "
        "class C public static method Main() "
        "  var r := new R(3); "
        "  var m: () -> string := r.Show; "
        "  var n: () -> string := lambda() => r.Show(); "
        "  Console.WriteLine(m()); "
        "  Console.WriteLine(n()); "
        "end Main; end C;",
        "zerofield_5_nonempty", "R3\nR3");
}

// Control D: two zero-field records stored in an Array. A zero-byte element
// type gives the array a stride of 0, so every element aliases the same
// address; the one-byte footprint makes each slot distinct. Reading back both
// ends must stay clean.
BEZEL_TEST(zero_field_record_capture_cell_spec_28_9,
           spec_28_9_array_of_zero_field_records_has_nonzero_stride) {
    expectClean(
        "module M; "
        "record Z() "
        "  public method Tag(): string return \"Z\"; end Tag; "
        "end Z; "
        "class C public static method Main() "
        "  using var a := new Array<Z>(); "
        "  var z := new Z(); "
        "  a.Add(z); "
        "  a.Add(z); "
        "  var r0 := a.Get(0); "
        "  var r1 := a.Get(1); "
        "  Console.WriteLine(r0.Tag()); "
        "  Console.WriteLine(r1.Tag()); "
        "end Main; end C;",
        "zerofield_6_array", "Z\nZ");
}
