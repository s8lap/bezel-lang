// fix #820 / §15.4 / §16: a collection literal assigned to a HEAP-collection
// target (`Array<T>`, `OwningArray<T>`, ...) in an assignment statement — most
// notably a constructor-body field assignment `_arr := [1, 2, 3]` — was silently
// dropped (the inline-array literal path only handles an `[N x elem]` ArrayType
// slot; a heap collection is a bare `ptr` slot). The field stayed null and the
// first use crashed (SIGSEGV). Fix: materialize a real heap collection and store
// the pointer, mirroring the field-initializer path.
//
// Value / disposal-count (JIT) tests — runJit returns the raw int.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §15.4: `Array<int>` field assigned a literal in the constructor body. 3 (was
// a SIGSEGV — the field was left null).
BEZEL_TEST(heap_collection_literal_assign_820, array_int_field_ctor_assign) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  private _arr: Array<int>; "
        "  public constructor() _arr := [1, 2, 3]; end constructor; "
        "  public method Len(): int => _arr.Count; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.Len(); "       // 3
        "end Main; end C;"), 3);
}

// §15.4: `Array<string>` field literal in the ctor body; element read works.
BEZEL_TEST(heap_collection_literal_assign_820, array_string_field_ctor_assign) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  private _arr: Array<string>; "
        "  public constructor() _arr := [\"a\", \"bb\"]; end constructor; "
        "  public method Get(): int => _arr[1].Length; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.Get(); "       // 2
        "end Main; end C;"), 2);
}

// §15.4 / §28.5: an `owned OwningArray<Cell>` field initialized via a literal in
// the ctor body disposes its owned elements when the owner is disposed (no leak
// in the ctor-init case — there is no prior value). Cell.n returns to 0.
BEZEL_TEST(heap_collection_literal_assign_820, owned_array_field_ctor_assign_disposes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class Box "
        "  private owned _arr: OwningArray<Cell>; "
        "  public constructor() _arr := [new Cell(), new Cell()]; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); end; "
        "  return Cell.n; "                                  // 0
        "end Main; end C;"), 0);
}

// §15.4: a heap-collection field reassigned a literal in a method body produces
// the right value (the store is no longer dropped). 5. (The old-value disposal
// for OWNING collection reassignment is a separate pre-existing gap, #824.)
BEZEL_TEST(heap_collection_literal_assign_820, array_field_method_reassign_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  private _arr: Array<int>; "
        "  public constructor() _arr := [1]; end constructor; "
        "  public method Reset(): int "
        "    _arr := [10, 20, 30, 40, 50]; "
        "    return _arr.Count; "
        "  end Reset; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.Reset(); "     // 5
        "end Main; end C;"), 5);
}

// Regression guard: a local-variable collection-literal initializer (which the
// local-decl path already handled) still works. 3.
BEZEL_TEST(heap_collection_literal_assign_820, local_array_initializer_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [7, 8, 9]; "
        "  return a.Count; "                                // 3
        "end Main; end C;"), 3);
}
