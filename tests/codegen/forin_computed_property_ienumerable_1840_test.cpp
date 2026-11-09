// Codegen / leak — #1840 / §29.2 / §28.5: a `for var x in obj.P` where `P` is a
// COMPUTED property returning a fresh owned `IEnumerable<T>` per read was
// treated as a NAMED (auto-property / field / local / param) source and the
// for-in therefore REFUSED to dispose the produced source frame. The for-in
// iterates a fresh per-read value (the getter is called at every read), so the
// source IS loop-owned (exactly like a CallExpr/NewExpr temporary) and must be
// disposed once at loop end. The bug surfaced as an LSan leak of the source
// iterator frame (32 bytes for a single-yield iterator).
//
// THE FIX. lowerIEnumerableBuiltinFor's source-disposal classification
// (LlvmBackend_Stmts.cpp) now distinguishes an auto-property (backed by a
// compiler-generated field → borrow, do not dispose) from a computed property
// (expression-body, block-body, `=>`-body → invoked at the call site, fresh
// owned value per read, dispose once). isAutoPropertySymbol is the discriminator
// and is shared by sema/codegen (a storage vs. computed provenance test that
// already covers every other site that needs it).
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; a leaked source
// frame flips r.ran to false. The exit code pins the correct enumeration result
// (10+20+30 = 60), so a premature free (UAF) would corrupt it as well as flip
// r.ran. // per §29.2, §28.5 (issue #1840)

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// ---------------------------------------------------------------------------
// // per §29.2, §28.5 (issue #1840)
// ---------------------------------------------------------------------------

// HEADLINE: the computed property `=> C.Gen()` returns a fresh owned
// IEnumerable<int> per read.
//
// fix #7762 (§28.6 bullet 5 + §28.3.6 clause 1, v0.355): the calls-mint
// classification now covers the ITERABLE half of the mint-announcing
// families, so `C.Gen()` — an UNMARKED `IEnumerable<int>` return — produces
// an OWNED FRAME at the call site (§28.3.6 clause 1: "A call whose declared
// return type ... is a mint-announcing type produces an owned frame at the
// call site"). §28.6 bullet 5 then rejects the GETTER that returns it:
// "A property or indexer getter may not return a freshly-produced owned value
// (axiom A4) ... This holds at every declared type, a mint-announcing one
// included ... and in every spelling: `get => _items.Drain()`, an
// expression-bodied property, and the fresh value first bound to a getter-local
// and returned (`var v := _items.Drain(); return v;`) are all this error. Expose
// the factory as a method returning `owned T` instead, which can hand the
// caller ownership."
//
// So the per-read frame this row used to observe at RUNTIME is now rejected at
// COMPILE time — the leak the row pinned is unreachable through this spelling
// (the `Task`-family twin of the same shape — `property T: Task<int> =>
// Task.FromResult(0);` — has been rejected by the same §28.6 rule since the
// Task row of the calls-mint set landed). The runtime coverage moves to the
// METHOD spelling below, which is §28.6's own named remedy and keeps the
// "a fresh per-read source frame is disposed once by the for-in" assertion.
BEZEL_TEST(forin_computed_property_ienumerable_1840,
           computed_ienumerable_property_rejected_by_a4_spec_28_6) {
    const std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 10; yield return 20; yield return 30; "
        "end Gen; end C; "
        "class Holder "
        "  public property Seq: IEnumerable<int> => C.Gen(); "
        "  public method Sum(): int "
        "    var total: int := 0; "
        "    for var x in Seq do total := total + x; end for; "
        "    return total; "
        "  end Sum; "
        "end Holder; "
        "class C2 public static method Main(): int "
        "  using var h := new Holder(); return h.Sum(); "
        "end Main; end C2;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a property or indexer getter may not return a freshly-produced "
           "owned value"));
}

// The METHOD spelling (§28.6 bullet 5's own remedy: "Expose the factory as a
// method returning `owned T` instead") — the per-read source frame is still
// fresh, still owned at the call site, and the for-in must dispose it exactly
// once (LSan-clean). Exit code pins 10+20+30 = 60 (a premature free would
// corrupt the yield values).
BEZEL_TEST(forin_computed_property_ienumerable_1840,
           method_spelling_source_frame_disposed_lsan_clean_29_2) {
    const std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 10; yield return 20; yield return 30; "
        "end Gen; end C; "
        "class Holder "
        "  public method Seq(): IEnumerable<int> => C.Gen(); "
        "  public method Sum(): int "
        "    var total: int := 0; "
        "    for var x in Seq() do total := total + x; end for; "
        "    return total; "
        "  end Sum; "
        "end Holder; "
        "class C2 public static method Main(): int "
        "  using var h := new Holder(); return h.Sum(); "
        "end Main; end C2;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);      // compiled + LSan-clean (source frame freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 60); // 10 + 20 + 30
}

// CONTROL (no regression): a NAMED LOCAL source (an `Array<int>` typed local
// bound to `IEnumerable<int>`) is the borrowing case the fix preserves: the
// loop pins the existing value (no fresh frame per read, no new frame to
// dispose). Enumerating 2+5 = 7 confirms the correct path. The fix only changes
// behavior for PropertySymbol sources (computed-vs-auto), so a plain local
// source must continue to work as it did pre-#1840.
// // per §29.2, §28.5
BEZEL_TEST(forin_computed_property_ienumerable_1840,
           named_local_ienumerable_source_still_borrowed_29_2) {
    const std::string src =
        "module M; "
        "class C public static method Make(): IEnumerable<int> "
        "  yield return 2; yield return 5; "
        "end Make; end C; "
        "class C2 public static method Main(): int "
        "  using var src := C.Make(); "
        "  var s := 0; for var x in src do s := s + x; end; "
        "  return s; "
        "end Main; end C2;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);  // 2 + 5
}

// CONTROL (no regression): a MEMBER-ACCESS computed property
// (`for var x in obj.P do …` where `P` is a computed property) goes through
// the SAME classification branch (MemberAccessExpr → PropertySymbol with a
// non-auto decl) and must also dispose the per-read source frame. The
// computed value 100+200+300 = 600 exceeds the 8-bit exit-code truncation
// (600 & 0xFF = 88), so the test pins the result via stdout (r.stdoutText)
// — the runAotChecked helper captures the program's stdout separately from
// the truncated exit code.
//
// fix #7762 (§28.6 bullet 5 + §28.3.6 clause 1, v0.355): the per-read frame
// of a computed property at a mint-announcing head is an OWNED frame at the
// call site now, so the getter that returns it is rejected by §28.6's A4
// rule (the `Task`-family twin has been rejected the same way since the Task
// row of the calls-mint set landed). The runtime coverage of the member-access
// face moves to the METHOD spelling row below.
BEZEL_TEST(forin_computed_property_ienumerable_1840,
           member_access_computed_property_rejected_by_a4_spec_28_6) {
    const std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 100; yield return 200; yield return 300; "
        "end Gen; end C; "
        "class Holder "
        "  public property Seq: IEnumerable<int> => C.Gen(); "
        "end Holder; "
        "class C2 public static method Main(): int "
        "  using var h := new Holder(); "
        "  var s := 0; "
        "  for var x in h.Seq do s := s + x; end; "
        "  Console.WriteLine(s.ToString()); "
        "  return 0; "
        "end Main; end C2;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a property or indexer getter may not return a freshly-produced "
           "owned value"));
}

// The METHOD spelling of the member-access face — the per-read source frame
// is disposed once at the loop's end (LSan-clean), and the printed "600"
// confirms 100+200+300=600 was reached (a premature free would corrupt the
// yield values mid-loop).
BEZEL_TEST(forin_computed_property_ienumerable_1840,
           member_access_method_spelling_disposed_lsan_clean_29_2) {
    const std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 100; yield return 200; yield return 300; "
        "end Gen; end C; "
        "class Holder "
        "  public method Seq(): IEnumerable<int> => C.Gen(); "
        "end Holder; "
        "class C2 public static method Main(): int "
        "  using var h := new Holder(); "
        "  var s := 0; "
        "  for var x in h.Seq() do s := s + x; end; "
        "  Console.WriteLine(s.ToString()); "
        "  return 0; "
        "end Main; end C2;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);     // LSan-clean (source frame freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // stdoutText carries the captured program stdout; the printed "600"
    // confirms 100+200+300=600 was reached (a premature free would corrupt
    // the yield values mid-loop).
    BEZEL_EXPECT_TRUE(r.stdoutText.find("600") != std::string::npos);
}
