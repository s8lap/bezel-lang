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

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── #4346 core: the exact verified repros from the issue ───────────────────

// §9.5 / §15.4 / #4346: `b1 with(Items := [])` where `Items: Array<IHolder>`.
// Empty literal targeting a heap Array<T>. Before fix: #1386 ICE on the
// with-replacement value. Expected: 0 (count of an empty heap Array).
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
        "    var b2 := b1 with(Items := []); "
        "    return b2.Items.Count; "
        "  end Main; "
        "end C;"), 0);
}

// §9.5 / §15.4 / #4346: `b1 with(Items := [null])` where `Items: Array<IHolder?>`.
// Non-empty literal with a nullable element type targeting a heap Array<T>.
// Before fix: same #1386 ICE. Expected: 42 (the first element is null).
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
        "    var b2 := b1 with(Items := [null]); "
        "    return (if b2.Items[0] = null then 42 else 0); "
        "  end Main; "
        "end C;"), 42);
}

// §9.5 / §15.4 / #4346: a non-empty Array<T> literal `[1,2,3]` as a `with`
// replacement. The heap materializer populates the new array (sum = 15).
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
        "    var b2 := b1 with(Items := [4, 5, 6]); "
        "    return b2.Items[0] + b2.Items[1] + b2.Items[2]; "
        "  end Main; "
        "end C;"), 15);
}

// §9.5 / §15.4 / #4346: an empty Dictionary<K,V> literal `[]` as a `with`
// replacement for a Dictionary field. The heap materializer's internal
// delegation routes empty-literal-with-dict-target to materializeHeapDictLiteral.
// Expected: 0 (count of an empty Dictionary).
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
        "    var b2 := b1 with(Map := []); "
        "    return b2.Map.Count; "
        "  end Main; "
        "end C;"), 0);
}

// §9.5 / §15.4 / #4346: a non-empty Dictionary literal `["a":1,"b":2]` as a
// `with` replacement. Expected: 3 (sum of values).
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
        "    var b2 := b1 with(Map := [\"a\": 1, \"b\": 2]); "
        "    return b2.Map[\"a\"] + b2.Map[\"b\"]; "
        "  end Main; "
        "end C;"), 3);
}

// §9.5 / §15.4 / #4346: the source record's UN-replaced fields stay intact
// after the `with`. Two-field record; `with` replaces only `Items`, `Tag`
// is copied from the source. The heap-collection literal in `Items` no longer
// ICEs; the `Tag` field keeps its source value via the unmodified copy.
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
        "    var b2 := b1 with(Items := []); "
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
