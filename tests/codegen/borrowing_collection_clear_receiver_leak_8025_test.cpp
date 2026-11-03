// Regression tests for #8025 -- the BORROWING collection family leaked its
// RECEIVER in the `.Clear()` CALL shape.
//
// THE DEFECT. `Mk(): owned Array<int>` + `Mk().Clear()` allocated the
// collection's BACKING and never freed it. The `isArr` block's `.Clear()` arm
// (`tryLowerCollectionInstanceCall`, LlvmBackend_Calls.cpp) is the ONE arm of
// the borrowing collection family that carried no inline reclaim, so an
// unbound `owned` borrowing collection consumed as this arm's receiver leaked
// its `bz_array_t` header and its backing buffer.
//
// MEASURED on the pre-fix parent (1e074eda) under the generated-code ASan/LSan
// leg, 100 iterations of `Mk().Clear()`:
//
//   "SUMMARY: AddressSanitizer: 14400 byte(s) leaked in 200 allocation(s)."
//
// — 8000 B of `bz_array_t` headers plus 6400 B of backing buffers, 100
// iterations x 2 allocations. The `Array<string>` twin read 20800 B / 200
// allocations and the `Stack<T>` / `Queue<T>` twins read 14400 B / 200 each:
// all three are array-backed, so all three land on the SAME arm.
//
// The sibling faces were already clean and MUST STAY clean -- they are the
// rows that would catch a double free:
//
//   * the `isDict` block's `.Clear()` arm (Dictionary / SortedDictionary) has
//     carried `freeUnboundOwnedBorrowingArrayTemp` since #6419;
//   * the Set-family arms (`Set` / `SortedSet`) have carried it since #6394;
//   * the `.Length` / `.Count` PROPERTY face has carried it since #1006(b).
//
// THE PROPERTY-READ SHAPE IS NOT THE FIX SITE. `Mk().Count` is CLEAN (0
// allocations) on the pre-fix compiler: that face has its own inline reclaim.
// The reclaim must therefore go AT THE ARM SITE, and NOT at the §28.3 K8
// boundary -- the borrowing family is already reclaimed INLINE by its own
// arms, so a boundary reclaim would FREE THE BACKING TWICE. That is why this
// is a different issue from #8009 (the OWNING family's half, which IS a
// boundary release, because an owning collection owes an element cascade and
// that release is not name-keyed).
//
// ── THE LEAK ORACLE (this is the point of the file) ────────────────────────
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// compiler and would prove nothing. Every row here measures a COUNT: the
// number of allocations LeakSanitizer reports as leaked, read out of LSan's own
// "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in <allocations>
// allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the four leak rows read 200 allocations (14400 B,
// 20800 B, 14400 B, 14400 B). The rows also run under `runAotChecked`, whose
// LSan leg turns a leak into a failed row, so the count and the verdict agree.
//
// Every row loops 100 times: a one-shot leak repro is a vacuous oracle.
//
// ── THE SOUNDNESS ROWS ─────────────────────────────────────────────────────
// * A BOUND receiver (`var a := Mk(); a.Clear();`) is the LOCAL's to dispose
//   at scope end; the arm must not also free it (that would be a double
//   free). The row asserts zero leaked allocations.
// * A BORROWED receiver (`Holder.Get()` returns the holder's own field) is not
//   the caller's to dispose at all (§28.1). The row asserts zero leaked
//   allocations and no sanitizer report.
// * The OWNING family (`OwningArray<Cell>`) is released by #8009 at the
//   §28.3 K8.1 BOUNDARY, and its element cascade is a DIFFERENT release (the
//   §28.11 element disposer, which depends on the ELEMENT type). The two
//   populations are disjoint by construction -- the borrowing helper declines
//   the `Owning*` heads -- so this row asserts that the owning family is
//   still clean after the borrowing family's arm-site reclaim was added: it
//   is the row that would go RED on a double free.
//
// Spec: §28.3 *Unbound owned temporaries* / K8.1 (v0.350), §28.9, §28.11,
// stdlib §4.4 (`Array<T>`), §4.6 / §4.7 (`Stack<T>` / `Queue<T>`),
// §4.8 (`Dictionary<K,V>`), §4.10 (`Set<T>`), §28.1 (a borrow is never
// disposed).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated -- the sanitizer report
// the AOT harness embeds in the diagnostic message when LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// THE ORACLE. `leakedAllocations` is the COUNT of allocations LeakSanitizer
// reports as leaked: 0 when there is no SUMMARY line (LSan prints one only
// when it finds leaks), and the summary's own number when there is one. The
// pre-fix compiler produced 200 here for the array-backed rows (14400 B for
// `Array<int>` / `Stack<int>` / `Queue<int>`, 20800 B for `Array<string>`);
// the fixed compiler produces 0.
//
// `leakedBytes` is the same measurement in bytes.
//
// The row also asserts that the program ran to completion with the expected
// output and that NO sanitizer report of any kind is present, so a double
// free (which ASan reports as "attempting double-free") fails the row too.
void expectNoLeak(const JitResult& r, const char* out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured (the pre-fix compiler reads 200 allocations / 14400
    // bytes here, from `bz_array_alloc`).
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    // No sanitizer report of any kind: a double free would land here.
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

}  // namespace

// THE ISSUE'S OWN REPRO, verbatim: `Mk(): owned Array<int>` whose result is
// the receiver of a further call (`Mk().Clear()`). 100 iterations; the
// receiver of every `Clear` is an unbound owned temporary that §28.3 K8.1
// disposes at the full expression's end -- with the BORROWING family's own
// backing free (`bz_array_free`), never a vtable dispatch.
//
// Pre-fix: 200 allocations / 14400 B. Post-fix: 0.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           array_call_receiver_loop_100_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Array<int>
    var a := new Array<int>();
    a.Add(7);
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// THE SAME ARM, a STRING element. The clear itself discharges each element's
// co-owned +1 (`bz_array_clear_disposing` with the string-release thunk), and
// it ERASES `count`, so the backing free that follows visits no element: the
// elements are released EXACTLY ONCE. This row is the one that would go RED
// on a double release of the string elements.
//
// Pre-fix: 200 allocations / 20800 B. Post-fix: 0.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           array_string_call_receiver_loop_100_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Array<string>
    var a := new Array<string>();
    a.Add("x");
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// THE SAME ARM, the other two array-backed borrowing heads (`Stack<T>` /
// `Queue<T>`, stdlib §4.6 / §4.7). They are array-backed, so they land on the
// SAME `.Clear()` arm -- one row each, because each head's release is a
// different runtime shape.
//
// Pre-fix: 200 allocations / 14400 B each. Post-fix: 0.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           stack_call_receiver_loop_100_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Stack<int>
    var s := new Stack<int>();
    s.Push(7);
    return s;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           queue_call_receiver_loop_100_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Queue<int>
    var q := new Queue<int>();
    q.Enqueue(7);
    return q;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// TWO receivers in one function, in two statements -- each must be released
// EXACTLY ONCE, at its own boundary. Two slots, two reclaims.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           array_two_receiver_statements_loop_100_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Array<int>
    var a := new Array<int>();
    a.Add(7);
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// ── THE SIBLING SHAPES: they ALREADY reclaimed inline, and must STAY clean ─
//
// The `isDict` block's `.Clear()` arm (the #6419 arm) reclaims through the
// same helper. If the borrowing family were freed at the K8 boundary AS WELL
// as inline, THIS row would report a double free (and the leak count would
// not be the only failure). It is clean on the pre-fix compiler and must stay
// clean.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           dictionary_call_receiver_stays_clean_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Dictionary<string, int>
    var d := new Dictionary<string, int>();
    d.Add("k", 1);
    return d;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// The Set-family arms (#6394) are the other inline reclaims.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           set_call_receiver_stays_clean_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Set<int>
    var s := new Set<int>();
    s.Add(7);
    return s;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// The PROPERTY-READ face (`Mk().Count`) -- already clean pre-fix, and the
// reason the fix belongs at the `.Clear()` ARM and not at the K8 boundary.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           array_property_receiver_stays_clean_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Array<int>
    var a := new Array<int>();
    a.Add(7);
    return a;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Mk().Count;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// ── THE SOUNDNESS ROWS ─────────────────────────────────────────────────────
//
// A BOUND receiver: the local owns it and disposes it at the scope's end
// (§28.3's auto-drop for an owned local). The arm must NOT also free it --
// that would be a double free, which ASan reports and the row fails.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           bound_array_receiver_is_not_freed_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Array<int>
    var a := new Array<int>();
    a.Add(7);
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      var a := Mk();
      a.Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// A BORROWED receiver: `Holder.Get()` returns the holder's own field -- a
// borrow, not an `owned` transfer (§28.1). The collection is the HOLDER's to
// dispose, so the arm must leave it alone. This row is GREEN on the pre-fix
// compiler too (the borrow was never claimed); its job is to STAY green.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           borrowed_array_receiver_is_not_freed_spec_28_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Holder
  private owned _f: Array<int> := new Array<int>();
  public method Get(): Array<int> => _f;
end Holder;
class C
  public static method Main(): int
    using var h := new Holder();
    for var i in 0..<100 do
      h.Get().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// The OWNING family (`OwningArray<Cell>`): #8009 releases it at the §28.3
// K8.1 BOUNDARY, with the §28.11 element cascade -- a DIFFERENT release from
// the borrowing family's bare backing free. The two populations are disjoint
// by construction (the borrowing helper declines the `Owning*` heads), so this
// row must stay CLEAN: it is the row that goes RED if the arm-site reclaim
// were to reach the owning family as well (a double free of the backing, and
// a double dispose of the elements).
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           owning_array_call_receiver_stays_clean_spec_28_3_k8_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// ── THE VALUE ROW: the fix must not change what `.Clear()` DOES ────────────
//
// The leak oracle above says "no bytes were lost"; this row says "and the
// collection is still empty afterwards". A fix that freed the backing too
// early (before the clear) would leave a dangling array, and a fix that
// skipped the clear would leave the element in place -- this row pins the
// VALUE, so neither can pass.
BEZEL_TEST(borrowing_collection_clear_receiver_leak_8025,
           clear_empties_the_borrowing_array_spec_4_4) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Mk(): owned Array<int>
    var a := new Array<int>();
    a.Add(7);
    a.Add(9);
    return a;
  end Mk;
  public static method Main(): int
    var a := Mk();
    Console.WriteLine(a.Count);
    a.Clear();
    Console.WriteLine(a.Count);
    return 0;
  end Main;
end C;
)BZL"),
                 "2\n0\n");
}
