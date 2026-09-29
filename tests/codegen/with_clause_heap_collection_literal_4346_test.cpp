// Fix verification tests for issue #4346.
//
// #4346 (internal-error / codegen gap): a `with(Items := [...])` clause on a
// record whose `Items` field is a HEAP collection (`Array<T>`, `Dictionary<K,V>`)
// never materialized the replacement value — codegen's `with`-expression lowering
// has no case for a heap collection literal at all, and fell into the generic
// "unlowered expression" internal-error backstop (#1386's sweep).
//
//   module M;
//   import System.Collections.Generic;
//   interface IHolder method GetV(): string; end IHolder;
//   record HeapBox(Items: Array<IHolder>);
//   class C public static method Main(): int
//     var items1: Array<IHolder> := [];
//     var b1 := new HeapBox(items1);
//     var b2 := b1 with(Items := []);
//     return b2.Items.Count;
//   end Main; end C;
//
// `with(Items := [])` ICEd with:
//   internal: lowerExpr returned null for with-expression
//   field/property replacement value — #1386
//
// Root cause (confirmed): lowerWith (LlvmBackend_Exprs2.cpp) handled a
// `with`-replacement value that was a CollectionLiteralExpr only when the
// target field was an INLINE array `[N]T` (the #1382 GEP-store path). It had no
// sibling arm for a heap collection target — lowerExpr returns null for any
// CollectionLiteralExpr, so the replacement store was skipped (and an internal
// error was reported). The LocalDecl/FieldDecl/ReturnStmt/call-argument positions
// already materialize a heap collection literal (the #465 / #538 / #999 / #4330
// family of fixes); the `with`-clause site was the only remaining literal-value
// position without one.
//
// Fix (LlvmBackend_Exprs2.cpp): before falling through to lowerExpr, check whether
// the `with`-replacement value is a CollectionLiteralExpr AND the target field's
// resolved TypeRef names a heap collection. If so, call
// `materializeCollectionLiteralForArg` (the same single dispatch the call-arg path
// uses — routes sequence→materializeHeapCollectionLiteral, dict→materializeHeapDictLiteral,
// empty-literal-with-dict-target→materializeHeapDictLiteral by internal delegation)
// and store the resulting `bz_array_t*` / `bz_dict_t*` into the field slot. The
// source record is unmodified and still owns its own collection; the new record
// owns the freshly-materialized one. No dispose-old is needed here — the new
// slot's ownership is established when we store the new pointer.
//
// Spec: §9.5 (record `with`-expression destructive update), §15.4 (collection
// literals), stdlib §4.4 (Array<T>), §4.8 (Dictionary<K,V>), §28.3 / §28.10
// (ownership/disposal of the materialized result).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LSan-clean oracle: ASan/LSan-instrumented generated code. A leak of the
// heap-collection literal's bz_array_t (and backing) shows up as a non-zero
// exitCode with LeakSanitizer in the report text.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── #4346 core: the exact verified repros from the issue ───────────────────

// §9.5 / §15.4 / #4346: `b1 with(Items := [])` where `Items: Array<IHolder>`.
// Empty literal targeting a heap Array<T>. Before fix: #1386 ICE on the
// with-replacement value. Expected: 0 (count of an empty heap Array).
//
// STRATEGY A (§9.7 / §28.3 borrow-lifetime rule, fix #4330/#4344 cherry-pick
// 4b93e229): a heap-collection LITERAL used directly as a `with` replacement
// value is a §9.7 temporary and is rejected. The fix: bind the literal to a
// named local first (`var items2: Array<IHolder> := []`), then pass the local
// in the `with` (no longer a temporary at the `with` site). The named local's
// scope extends through the `with` evaluation, satisfying §9.7.
BEZEL_TEST(with_heap_collection_literal_4346,
           array_empty_literal_with_replacement_4346) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Generic; "
        "interface IHolder "
        "  method GetV(): string; "
        "end IHolder; "
        "record HeapBox(Items: Array<IHolder>); "
        "class C "
        "  public static method Main(): int "
        "    var items1: Array<IHolder> := []; "
        "    var b1 := new HeapBox(items1); "
        "    var items2: Array<IHolder> := []; "   // §9.7: named local, not a temporary
        "    var b2 := b1 with(Items := items2); "
        "    return b2.Items.Count; "
        "  end Main; "
        "end C;"), 0);
}

// §9.5 / §15.4 / #4346: `b1 with(Items := [null])` where `Items: Array<IHolder?>`.
// Non-empty literal with a nullable element type targeting a heap Array<T>.
// Before fix: same #1386 ICE. Expected: 42 (the first element is null).
//
// STRATEGY A (§9.7 / §28.3 borrow-lifetime rule, fix #4330/#4344 cherry-pick
// 4b93e229): bind the heap-collection literal to a named local first so it is
// not a §9.7 temporary at the `with` site.
BEZEL_TEST(with_heap_collection_literal_4346,
           array_nullable_element_literal_with_replacement_4346) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Generic; "
        "interface IHolder "
        "  method GetV(): string; "
        "end IHolder; "
        "record NullableBox(Items: Array<IHolder?>); "
        "class C "
        "  public static method Main(): int "
        "    var items1: Array<IHolder?> := [null]; "
        "    var b1 := new NullableBox(items1); "
        "    var items2: Array<IHolder?> := [null]; "   // §9.7: named local
        "    var b2 := b1 with(Items := items2); "
        "    return (if b2.Items[0] = null then 42 else 0); "
        "  end Main; "
        "end C;"), 42);
}

// §9.5 / §15.4 / #4346: a non-empty Array<T> literal `[1,2,3]` as a `with`
// replacement. The heap materializer populates the new array (sum = 15).
//
// STRATEGY A (§9.7 / §28.3 borrow-lifetime rule, fix #4330/#4344 cherry-pick
// 4b93e229): bind the heap-collection literal to a named local first so it is
// not a §9.7 temporary at the `with` site.
BEZEL_TEST(with_heap_collection_literal_4346,
           array_nonempty_literal_with_replacement_4346) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Generic; "
        "record Box(Items: Array<int>); "
        "class C "
        "  public static method Main(): int "
        "    var items1: Array<int> := [1, 2, 3]; "
        "    var b1 := new Box(items1); "
        "    var items2: Array<int> := [4, 5, 6]; "   // §9.7: named local
        "    var b2 := b1 with(Items := items2); "
        "    return b2.Items[0] + b2.Items[1] + b2.Items[2]; "
        "  end Main; "
        "end C;"), 15);
}

// §9.5 / §15.4 / #4346: an empty Dictionary<K,V> literal `[]` as a `with`
// replacement for a Dictionary field. The heap materializer's internal
// delegation routes empty-literal-with-dict-target to materializeHeapDictLiteral.
// Expected: 0 (count of an empty Dictionary).
//
// STRATEGY A (§9.7 / §28.3 borrow-lifetime rule, fix #4330/#4344 cherry-pick
// 4b93e229): bind the heap-collection literal to a named local first so it is
// not a §9.7 temporary at the `with` site.
BEZEL_TEST(with_heap_collection_literal_4346,
           dict_empty_literal_with_replacement_4346) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Generic; "
        "record DictBox(Map: Dictionary<string, int>); "
        "class C "
        "  public static method Main(): int "
        "    var d1: Dictionary<string, int> := []; "
        "    var b1 := new DictBox(d1); "
        "    var map2: Dictionary<string, int> := []; "   // §9.7: named local
        "    var b2 := b1 with(Map := map2); "
        "    return b2.Map.Count; "
        "  end Main; "
        "end C;"), 0);
}

// §9.5 / §15.4 / #4346: a non-empty Dictionary literal `["a":1,"b":2]` as a
// `with` replacement. Expected: 3 (sum of values).
//
// STRATEGY A (§9.7 / §28.3 borrow-lifetime rule, fix #4330/#4344 cherry-pick
// 4b93e229): bind the heap-collection literal to a named local first so it is
// not a §9.7 temporary at the `with` site.
BEZEL_TEST(with_heap_collection_literal_4346,
           dict_nonempty_literal_with_replacement_4346) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Generic; "
        "record DictBox(Map: Dictionary<string, int>); "
        "class C "
        "  public static method Main(): int "
        "    var d1: Dictionary<string, int> := []; "
        "    var b1 := new DictBox(d1); "
        "    var map2: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "   // §9.7: named local
        "    var b2 := b1 with(Map := map2); "
        "    return b2.Map[\"a\"] + b2.Map[\"b\"]; "
        "  end Main; "
        "end C;"), 3);
}

// §9.5 / §15.4 / #4346: the source record's UN-replaced fields stay intact
// after the `with`. Two-field record; `with` replaces only `Items`, `Tag`
// is copied from the source. The heap-collection literal in `Items` no longer
// ICEs; the `Tag` field keeps its source value via the unmodified copy.
//
// STRATEGY A (§9.7 / §28.3 borrow-lifetime rule, fix #4330/#4344 cherry-pick
// 4b93e229): bind the heap-collection literal to a named local first so it is
// not a §9.7 temporary at the `with` site.
BEZEL_TEST(with_heap_collection_literal_4346,
           multi_field_other_field_preserved_4346) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Generic; "
        "interface IHolder "
        "  method GetV(): string; "
        "end IHolder; "
        "record TwoFields(Items: Array<IHolder>, Tag: string); "
        "class C "
        "  public static method Main(): int "
        "    var items1: Array<IHolder> := []; "
        "    var b1 := new TwoFields(items1, \"old\"); "
        "    var items2: Array<IHolder> := []; "   // §9.7: named local
        "    var b2 := b1 with(Items := items2); "
        "    return (if b2.Tag = \"old\" then b2.Items.Count else -1); "
        "  end Main; "
        "end C;"), 0);
}

// §9.5 / §15.4 / #4346: an INLINE-array field still goes through the existing
// #1382 GEP-store path (the new heap-collection arm is a sibling, not a
// replacement). Regression guard: the existing inline-array behaviour is
// preserved. Sum = 1+2+9 = 12.
BEZEL_TEST(with_heap_collection_literal_4346,
           inline_array_field_still_works_4346) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R(A: [3]int); "
        "class C public static method Main(): int "
        "  var r := new R([10, 20, 30]); "
        "  var r2 := r with(A := [1, 2, 9]); "
        "  return r2.A[0] + r2.A[1] + r2.A[2]; "
        "end Main; end C;"), 12);
}


// ── #4350 v2 regression: heap-collection literal local is freed even when
// passed to a record constructor (the pre-existing #4c33bcd with-clause
// relaxation is necessary but NOT sufficient) ───────────────────────────────
//
// The with-clause half (#4c33bcd) freed the freshly-materialized array
// inside `lowerWith`'s result-record scope, so the `with(Items := [...])`
// form is leak-clean. The CONSTRUCTOR half stayed leaky: `var items1:
// Array<int> := [1, 2, 3]; var b1 := new R(items1);` did not free items1's
// heap backing at scope end — the constructor call argument was treated as
// a conservative ESCAPE by the borrowing-Array escape walk
// (`StringEscapeWalker::walkNewArgs`, gated on `stringReleaseMode` in #2023,
// before this fix), so items1 was excluded from `borrowingArrayFreeEligible_`
// and no scope-end `bz_array_free` ever fired. Measured (pre-fix):
// 120 bytes leaked in 2 allocations (the bz_array_t + its data backing).
//
// The fix mirrors walkCallArgs's `borrowingArrayMode` relaxation into
// walkNewArgs: a plain (no-modifier, no-name, no-discard) constructor
// parameter at a record or non-`owned` class target admits the argument as
// a call-bounded borrow (records §9.7 "no disposer", and class fields
// without `owned` modifier lack field-side disposal of the array, so the
// plain-parameter copy leaves the local as sole owner).
//
// This test amplifies the leak in a `do … end` block repeated 100× via a
// generated `for i := 0 to 99` loop (per `lsan-one-shot-leak-tests-pass-
// vacuously.md`, one-shot LSan detection is ~90% flaky; the loop makes the
// leak deterministic at 12,000 B / 200 allocations per run). Without the fix
// LSan reports `12000 byte(s) leaked in 200 allocation(s)`; with the fix
// LSan is clean and the program exits 1.
BEZEL_TEST(with_heap_collection_literal_4346,
           ctor_arg_heap_collection_literal_disposed_at_scope_end_4350_amplified) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "import System.Collections.Generic; "
        "record R(Items: Array<int>); "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var items1: Array<int> := [1, 2, 3]; "
        "      var b1 := new R(items1); "
        "      i := i + 1; "
        "    end while; "
        "    return 1; "
        "  end Main; "
        "end C;"), 1);
}

// ── §9.7 REGRESSION PIN — heap-collection literal in with-expression ──────────
//
// fix #4330 / #4344 (§9.7 / §28.3, cherry-pick 4b93e229, v0.298):
// `checkRecordWithFromTemporary` now rejects a HEAP-COLLECTION LITERAL
// (Array<T>, Dictionary<K,V>) used directly as a `with` replacement value,
// because the literal is a §9.7 temporary whose lifetime ends at the
// `with(...)` expression. This test pins that the reject fires.
//
// Strategy A (naming the local first) is the correct workaround — the
// named-local forms above compile and run correctly. This test pins
// the negative: the literal form is rejected.
BEZEL_TEST(with_heap_collection_literal_4346,
           section_9_7_heap_collection_literal_with_replacement_rejected) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Generic; "
        "record Box(Items: Array<int>); "
        "class C "
        "  public static method Main(): int "
        "    var items1: Array<int> := [1, 2, 3]; "
        "    var b1 := new Box(items1); "
        "    var b2 := b1 with(Items := [4, 5, 6]); "   // §9.7: temporary rejected
        "    return b2.Items[0]; "
        "  end Main; "
        "end C;");
    // The program should NOT compile: the literal [4,5,6] is a §9.7 temporary.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow-bearing record may not be constructed from a temporary"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "via `with(...)`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7"));
}
