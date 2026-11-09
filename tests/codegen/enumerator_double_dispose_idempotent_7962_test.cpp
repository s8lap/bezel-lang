// Codegen: an enumerator's `Dispose()` is idempotent in CHECKED disposal mode
// (issue #7962 — spec §28.2 "The `IDisposable` contract", v0.350).
//
// §28.2: "`Dispose()` on any class instance is idempotent: the first call
// performs the disposal; subsequent calls are no-ops", and the
// checked-disposal paragraph: "the split between logical disposal and
// physical reclamation (§28.12) gives the implementation latitude to defer
// reclaiming a disposed instance's memory in checked mode (quarantine, delayed
// free) so that the flag remains readable after disposal".
//
// WHAT WAS WRONG. An `IEnumerator<T>`'s per-instance disposed flag IS the
// state sentinel `-2` stored in the iterator frame (the `dsp.idem` guard in
// `emitDisposeBody`, LlvmBackend_Iterators.cpp). The frame was FREED
// unconditionally at the end of the Dispose body, so the SECOND `Dispose()`'s
// entry guard LOADED that sentinel out of freed memory.
//
//     public static method Main(): int
//       var a := G.Gen().GetEnumerator();
//       if a.MoveNext() then Console.WriteLine("mv"); end;
//       a.Dispose();
//       a.Dispose();          // §28.2: a no-op
//       Console.WriteLine("closed");
//       Console.WriteLine("end");
//       return 0;
//     end Main;
//
//     $ bezelc --sanitize=address --checked-disposal -o p p.bzl && ./p
//     mv
//     fin
//     ==3186006==ERROR: AddressSanitizer: heap-use-after-free ...
//     READ of size 8 at 0x7bdcda9e0050 thread T0
//         #0 ... in main
//     0x7bdcda9e0050 is located 0 bytes inside of 40-byte region
//     freed by thread T0 here:
//         #1 ... in bz$iter$M.G$Gen$CloneDispose
//     previously allocated by thread T0 here:
//         #1 ... in bz$iter$M.G$Gen$GetEnumerator
//     SUMMARY: AddressSanitizer: heap-use-after-free in main
//
// WHY THE CALL SITE COULD NOT SAVE IT. The explicit-`Dispose()` dispatch
// (LlvmBackend_Calls2.cpp) deliberately does NOT null the receiver slot in
// checked mode — "Checked mode does not null the slot — the object is
// quarantined — so `obj` stays non-null there and the re-dispatch is a no-op
// via the run-once guard in the Dispose body". The run-once guard is the `-2`
// sentinel, so it can only work if the frame SURVIVES the first Dispose.
//
// THE FIX. The Dispose body's frame reclamation now asks the runtime's
// checked-disposal escaped set (`bz_cd_may_free`) whether a surviving
// reference can still reach the frame; if one can, the frame is QUARANTINED
// (not freed) so the `-2` sentinel stays readable and the second `Dispose()`
// is the genuine no-op §28.2 requires. A frame no surviving reference can
// reach — the per-iteration clone of a `for` loop, whose slot the compiler
// nulls at the end of the iteration, and whose disposal is compiler-inserted
// and therefore exactly-once — is still freed, so the quarantine does not
// turn every checked-mode enumeration into a retained allocation.
//
// Rows:
//   1. The issue's repro: a second explicit `Dispose()` on an owner
//      enumerator local, CHECKED mode under generated-code ASan. Pre-fix a
//      heap-use-after-free; post-fix the run completes and the iterator's
//      `finally` body runs EXACTLY once (the second `Dispose()` is a no-op).
//   2. The factory twin: the enumerator comes from a `static method` rather
//      than from `GetEnumerator()` on a fresh call.
//   3. The field twin: the receiver is `this.En!`, an `owned` field read.
//   4. CONTROL — the ordinary-class twin: a second `Dispose()` on an ordinary
//      class local, CHECKED mode. Unchanged (the class path quarantines the
//      block and the guard reads the disposed flag).
//   5. CONTROL — the same double `Dispose()` in UNCHECKED mode. Unchanged
//      (the call site nulls the slot, so the second call is skipped there).
//   6. NO-LEAK CONTROL: a `for` loop over an iterator method, CHECKED mode
//      under ASan WITH LeakSanitizer. The per-iteration clone must still be
//      reclaimed — the quarantine is bounded by the escaped set, so this row
//      fails if the fix quarantines unconditionally.
//   7. CONTROL — the ordinary-class twin of row 4 in UNCHECKED mode.
//
// per §28.2 (issue #7962)

#include <string>

#include "support/aot_support.hpp"   // runAotCheckedImpl — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::codegen::CodegenOptions;
using bezel::test::JitResult;
using bezel::test::runAotCheckedImpl;

namespace {

// Checked-disposal mode under generated-code ASan, leaks NOT checked (a
// quarantined block is retained by design — §28.2's latitude). A
// freed-then-read block is a heap-use-after-free report, which surfaces as
// ran=false. Same recipe as tests/codegen/
// checked_disposal_bounded_retention_4602_test.cpp's `checkedAsanOut`.
std::string checkedAsanOut(const std::string& src) {
    CodegenOptions opts;
    opts.checkedDisposal = true;  // §28.2 checked-disposal MODE
    auto r = runAotCheckedImpl(src, "runAotCheckedLenient",
                               /*detectLeaks=*/false, opts);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}

// The same, but with LeakSanitizer ON — for the row that proves the
// quarantine is BOUNDED (the per-iteration clone is still reclaimed).
std::string checkedAsanLeakOut(const std::string& src) {
    CodegenOptions opts;
    opts.checkedDisposal = true;  // §28.2 checked-disposal MODE
    auto r = runAotCheckedImpl(src, "runAotChecked",
                               /*detectLeaks=*/true, opts);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}

// Unchecked (release) mode under generated-code ASan — the control mode.
std::string uncheckedAsanOut(const std::string& src) {
    CodegenOptions opts;
    auto r = runAotCheckedImpl(src, "runAotCheckedLenient",
                               /*detectLeaks=*/false, opts);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}

// The iterator under test: a two-yield generator whose `finally` prints
// "fin" — the observable that says the Dispose ran its cleanup chain, and
// (run once per disposal) that a REPEATED Dispose did not.
const char* kGen =
    "class G\n"
    "  public static method Gen(): IEnumerable<int>\n"
    "    try\n"
    "      yield return 1;\n"
    "      yield return 2;\n"
    "    finally\n"
    "      Console.WriteLine(\"fin\");\n"
    "    end;\n"
    "  end Gen;\n"
    "end G;\n";

// Row 1's program: an owner enumerator local, disposed twice.
const std::string kDoubleDisposeLocal =
    std::string("module M;\n") + kGen +
    "class C\n"
    "  public static method Main(): int\n"
    "    var a := G.Gen().GetEnumerator();\n"
    "    if a.MoveNext() then Console.WriteLine(\"mv\"); end;\n"
    "    a.Dispose();\n"
    "    a.Dispose();\n"
    "    Console.WriteLine(\"closed\");\n"
    "    Console.WriteLine(\"end\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Row 2's program: the same, but the enumerator comes from a factory method.
const std::string kDoubleDisposeFactory =
    std::string("module M;\n") + kGen +
    "class C\n"
    "  public static method Mk(): IEnumerator<int> => G.Gen().GetEnumerator();\n"
    "  public static method Main(): int\n"
    "    var a := C.Mk();\n"
    "    if a.MoveNext() then Console.WriteLine(\"mv\"); end;\n"
    "    a.Dispose();\n"
    "    a.Dispose();\n"
    "    Console.WriteLine(\"closed\");\n"
    "    Console.WriteLine(\"end\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Row 3's program: the receiver is `this.En!`, an `owned` field read, and the
// disposal is written in the field's DECLARING type (§28.6's receiver rule:
// "an `owned` field is disposable only through `this` (code of its declaring
// type)").
const std::string kDoubleDisposeField =
    std::string("module M;\n") + kGen +
    "class H\n"
    "  public owned En: IEnumerator<int>? := null;\n"
    "  public method Fill()\n"
    "    En := G.Gen().GetEnumerator();\n"
    "  end Fill;\n"
    "  public method CloseTwice()\n"
    "    this.En!.Dispose();\n"
    "    this.En!.Dispose();\n"
    "  end CloseTwice;\n"
    "end H;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    using var h := new H();\n"
    "    h.Fill();\n"
    "    if h.En!.MoveNext() then Console.WriteLine(\"mv\"); end;\n"
    "    h.CloseTwice();\n"
    "    Console.WriteLine(\"closed\");\n"
    "    Console.WriteLine(\"end\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Row 4 / row 7's program: the ordinary-CLASS twin — a second `Dispose()` on
// a plain class local. The class path quarantines the block and the
// per-instance disposed flag guards the repeat.
const std::string kDoubleDisposeClass =
    std::string("module M;\n"
    "class Res\n"
    "end Res;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var a := new Res();\n"
    "    a.Dispose();\n"
    "    a.Dispose();\n"
    "    Console.WriteLine(\"closed\");\n"
    "    Console.WriteLine(\"end\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n");

// Row 6's program: a `for` loop over an iterator method, 50 times. The
// per-iteration clone is disposed at the end of each iteration by
// compiler-inserted code (which NULLS its slot — so no surviving reference
// can reach it) and must therefore still be RECLAIMED: a leak here is the
// cost an unbounded quarantine would have introduced.
const std::string kForLoop =
    std::string("module M;\n") + kGen +
    "class C\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var i := 0;\n"
    "    while i < 50 do\n"
    "      for var v in G.Gen() do total := total + v; end;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(\"total=\" + total.ToString());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

}  // namespace

// ── Row 1, the issue's repro. Pre-fix: heap-use-after-free (the second
// `Dispose()` read the `-2` sentinel out of the freed frame). Post-fix the
// run completes, and "fin" appears EXACTLY once — the second `Dispose()` is
// §28.2's no-op, not a second cleanup. // per §28.2 (issue #7962)
BEZEL_TEST(codegen_enumerator_double_dispose_idempotent_7962,
           second_explicit_dispose_on_an_owner_enumerator_local_is_a_no_op_spec_28_2) {
    BEZEL_EXPECT_EQ(checkedAsanOut(kDoubleDisposeLocal),
                    "mv\nfin\nclosed\nend\n");
}

// ── Row 2, the FACTORY twin: the enumerator comes from a `static method`
// rather than from `GetEnumerator()` on a fresh call — the same frame, the
// same second Dispose. // per §28.2 (issue #7962)
BEZEL_TEST(codegen_enumerator_double_dispose_idempotent_7962,
           second_explicit_dispose_on_a_factory_enumerator_local_is_a_no_op_spec_28_2) {
    BEZEL_EXPECT_EQ(checkedAsanOut(kDoubleDisposeFactory),
                    "mv\nfin\nclosed\nend\n");
}

// ── Row 3, the FIELD twin: the receiver is `this.En!` — an `owned` field
// read, whose bare read lends (the receiver rule of §28.6 admits the
// disposal from the declaring type). The frame is the SAME shape as row 1's.
// per §28.2, §28.6 (issue #7962)
BEZEL_TEST(codegen_enumerator_double_dispose_idempotent_7962,
           second_explicit_dispose_on_an_owned_field_enumerator_is_a_no_op_spec_28_2) {
    BEZEL_EXPECT_EQ(checkedAsanOut(kDoubleDisposeField),
                    "mv\nfin\nclosed\nend\n");
}

// ── Row 4, CONTROL: the ordinary-CLASS twin of row 1, in checked mode.
// Unchanged by this fix — the class path already quarantines the block and
// the per-instance disposed flag guards the repeat. A fix that broke this
// would be an over-reach. // per §28.2 (issue #7962)
BEZEL_TEST(codegen_enumerator_double_dispose_idempotent_7962,
           second_explicit_dispose_on_an_ordinary_class_local_is_unchanged_spec_28_2) {
    BEZEL_EXPECT_EQ(checkedAsanOut(kDoubleDisposeClass),
                    "closed\nend\n");
}

// ── Row 5, CONTROL: the same double `Dispose()` in UNCHECKED (release)
// mode. Unchanged — the explicit-`Dispose()` call site nulls the receiver
// slot there, so the second call is skipped at the call site.
// per §28.2 (issue #7962)
BEZEL_TEST(codegen_enumerator_double_dispose_idempotent_7962,
           second_explicit_dispose_in_unchecked_mode_is_unchanged_spec_28_2) {
    BEZEL_EXPECT_EQ(uncheckedAsanOut(kDoubleDisposeLocal),
                    "mv\nfin\nclosed\nend\n");
}

// ── Row 6, the NO-LEAK CONTROL. The quarantine is bounded by the runtime's
// checked-disposal escaped set: a frame no surviving reference can reach —
// the per-iteration clone of a `for` loop, whose slot the compiler nulls at
// the end of the iteration — is still FREED. An unconditional quarantine
// would retain all 50 clones and LeakSanitizer would report them here.
// 50 * (1 + 2) = 150, and the generator's `finally` prints "fin" once per
// iteration (the for-in's end-of-iteration disposal runs it).
// per §28.2, §29.2 (issue #7962)
BEZEL_TEST(codegen_enumerator_double_dispose_idempotent_7962,
           checked_mode_for_loop_still_reclaims_each_iteration_clone_spec_28_2) {
    std::string expected;
    for (int i = 0; i < 50; ++i) expected += "fin\n";
    expected += "total=150\n";
    BEZEL_EXPECT_EQ(checkedAsanLeakOut(kForLoop), expected);
}

// ── Row 7, CONTROL: the ordinary-class twin of row 4 in UNCHECKED mode.
// per §28.2 (issue #7962)
BEZEL_TEST(codegen_enumerator_double_dispose_idempotent_7962,
           second_explicit_dispose_on_an_ordinary_class_local_in_unchecked_mode_is_unchanged_spec_28_2) {
    BEZEL_EXPECT_EQ(uncheckedAsanOut(kDoubleDisposeClass),
                    "closed\nend\n");
}
