// Codegen — §28.5/§28.11 (#893 #894): an owned OwningDictionary<K,V> CLASS FIELD
// disposes its V values on the containing object's disposal and on field
// reassignment.
//
// Root cause: `ownedCollectionFieldType` (LlvmBackend_impl.hpp) did not include
// dict types (Dictionary/OwningDictionary), so OwningDictionary fields were never
// added to the owned-field disposal list in emitOwnedFieldDisposals; and the
// field-store disposal path (lowerOwnedCollectionFieldStore) used bz_array_free
// with genericArgs[0] (the KEY) instead of bz_dict_free with genericArgs[1]
// (the VALUE) — so both the field-disposal pass and the field-store path leaked.
//
// Fix: (1) ownedCollectionFieldType now returns the TypeRef for dict fields;
// (2) emitOwnedFieldDisposals detects isDictTypeName, uses genericArgs[1] for
// the V disposer, and calls bz_dict_free; (3) lowerOwnedCollectionFieldStore
// does the same for field reassignment.
//
// Two kinds (mirroring owned_dict_disposal_test.cpp):
//   * destructor-count JIT tests (always run): counter proves disposal of the
//     class V elements exactly once, no double-free (counter never negative).
//   * OwningArray/OwningLinkedList/OwningStack field regression: must still dispose.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// --- #893: owned OwningDictionary field disposes class V values on disposal ---

// Box with `owned _dict: OwningDictionary<string, Cell>`. Two entries added in
// ctor. After the Box is disposed, Cell._n must be 0 (both V disposed exactly
// once). Before #893 the field-disposal pass skipped dict fields → Cell._n = 2.
const char* k893DictFieldDispose =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; "
    "class Box "
    "  private owned _dict: OwningDictionary<string, Cell>; "
    "  public constructor() "
    "    _dict := new OwningDictionary<string, Cell>(); "
    "    _dict.Add(\"x\", new Cell()); "
    "    _dict.Add(\"y\", new Cell()); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  do using var b := new Box(); end; "
    "  return Cell._n; "
    "end Main; end C;";

// #893 counter-encoded: each Cell assigned a unique id; counter = sum(id of
// live cells). After Box disposed, counter must be 0 (both disposed, not
// negative — no double-free). Cell id 10 and id 1 → _n starts at 0, ctor adds
// id, dtor subtracts id; after both disposed → 0.
const char* k893NoDoubleFree =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) _id := id; Cell._n := Cell._n + _id; end constructor; "
    "  destructor Cell._n := Cell._n - _id; end destructor; "
    "end Cell; "
    "class Box "
    "  private owned _dict: OwningDictionary<string, Cell>; "
    "  public constructor() "
    "    _dict := new OwningDictionary<string, Cell>(); "
    "    _dict.Add(\"a\", new Cell(10)); "
    "    _dict.Add(\"b\", new Cell(1)); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  do using var b := new Box(); end; "
    "  if Cell._n < 0 then return Cell._n * 1000 + 1; end; "
    "  return Cell._n; "
    "end Main; end C;";

// --- #894: reassigning an owned OwningDictionary field disposes OLD values ---

// Box with `owned _d`, one entry. Replace() creates a new dict with a different
// entry and assigns `_d := d2`. After Replace, old Cell disposed (Cell._n = 1,
// new one alive). After Box disposed, new Cell also disposed (Cell._n = 0).
const char* k894DictFieldReassign =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; "
    "class Box "
    "  private owned _d: OwningDictionary<string, Cell>; "
    "  public constructor() "
    "    _d := new OwningDictionary<string, Cell>(); "
    "    _d.Add(\"k\", new Cell()); "
    "  end constructor; "
    "  public method Replace() "
    "    var d2 := new OwningDictionary<string, Cell>(); "
    "    d2.Add(\"k2\", new Cell()); "
    "    _d := d2; "
    "  end Replace; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  do "
    "    using var b := new Box(); "
    "    b.Replace(); "
    "    var mid := Cell._n; "
    "    if mid <> 1 then return mid * 100 + 1; end; "
    "  end; "
    "  return Cell._n; "
    "end Main; end C;";

// --- Value-typed V: rejected at compile per stdlib §4.2/#1327 ---

// stdlib §4.2/#1327: OwningDictionary<string, int> is now rejected at the
// instantiation site because int is a value type with no disposal obligation.
// Kept as a REJECTED-AT-COMPILE test to document the new behaviour.
const char* kValueVNoCrash =
    "module M; "
    "class Box "
    "  private owned _dict: OwningDictionary<string, int>; "
    "  public constructor() "
    "    _dict := new OwningDictionary<string, int>(); "
    "    _dict.Add(\"x\", 42); "
    "    _dict.Add(\"y\", 99); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  do using var b := new Box(); end; "
    "  return 0; "
    "end Main; end C;";

// --- Borrowing dict field (not owned): values NOT disposed, no crash ---

// A non-owned Dictionary<string, Cell> field must NOT dispose values on Box
// disposal (only the owner of the values disposes them). The Box is disposed;
// the values (held in a using-var outer scope) survive: Cell._n stays 2.
const char* kBorrowingDictFieldNoCascade =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; "
    "class Box "
    "  private _dict: Dictionary<string, Cell>; "
    "  public constructor(d: Dictionary<string, Cell>) _dict := d; end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  using var c1 := new Cell(); "
    "  using var c2 := new Cell(); "
    "  using var d := new Dictionary<string, Cell>(); "
    "  d.Add(\"a\", c1); "
    "  d.Add(\"b\", c2); "
    "  do using var b := new Box(d); end; "
    "  if Cell._n <> 2 then return Cell._n * 10 + 1; end; "
    "  return 0; "
    "end Main; end C;";

// --- Regression: OwningArray field still disposes class elements ---

const char* kOwnedArrayFieldRegression =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; "
    "class Box "
    "  private owned _arr: OwningArray<Cell>; "
    "  public constructor() "
    "    _arr := new OwningArray<Cell>(); "
    "    _arr.Add(new Cell()); "
    "    _arr.Add(new Cell()); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  do using var b := new Box(); end; "
    "  return Cell._n; "
    "end Main; end C;";

// --- Regression: OwningLinkedList field still disposes class elements ---

const char* kOwnedLLFieldRegression =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; "
    "class Box "
    "  private owned _ll: OwningLinkedList<Cell>; "
    "  public constructor() "
    "    _ll := new OwningLinkedList<Cell>(); "
    "    _ll.AddLast(new Cell()); "
    "    _ll.AddLast(new Cell()); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  do using var b := new Box(); end; "
    "  return Cell._n; "
    "end Main; end C;";

// --- Regression: #824 OwningArray field reassignment disposes old V (existing) ---

const char* k824OwnedArrayReassignRegression =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; "
    "class Box "
    "  private owned _arr: OwningArray<Cell>; "
    "  public constructor() "
    "    _arr := new OwningArray<Cell>(); "
    "    _arr.Add(new Cell()); "
    "  end constructor; "
    "  public method Replace() "
    "    var a2 := new OwningArray<Cell>(); "
    "    a2.Add(new Cell()); "
    "    _arr := a2; "
    "  end Replace; "
    "  destructor end destructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  do "
    "    using var b := new Box(); "
    "    b.Replace(); "
    "    var mid := Cell._n; "
    "    if mid <> 1 then return mid * 100 + 1; end; "
    "  end; "
    "  return Cell._n; "
    "end Main; end C;";

}  // namespace

// ---- #893: owned OwningDictionary field disposes class V on object disposal ----
// §28.5/§28.11, #893

BEZEL_TEST(codegen_owned_dict_field_893, field_disposes_class_v_on_object_disposal) {
    // Both V values disposed once when Box is disposed → Cell._n = 0.
    // Before #893: field-disposal pass skipped dict fields → Cell._n = 2.
    BEZEL_EXPECT_EQ(run(k893DictFieldDispose), 0);
}

BEZEL_TEST(codegen_owned_dict_field_893, two_entry_no_double_free) {
    // Id-encoded counter (10+1=11 on ctor, 11 subtracted on dispose). After Box
    // disposed: Cell._n must be exactly 0, never negative (no double-free).
    // Before #893: counter stayed 11 (both leaked).
    BEZEL_EXPECT_EQ(run(k893NoDoubleFree), 0);
}

// ---- #894: owned OwningDictionary field reassignment disposes OLD values ------
// §28.5/§28.11, #894

BEZEL_TEST(codegen_owned_dict_field_894, field_reassign_disposes_old_values) {
    // After Replace(): old Cell disposed (Cell._n = 1), new Cell alive.
    // After Box disposed: new Cell also disposed (Cell._n = 0).
    // Before #894: old Cell leaked after Replace → Cell._n = 2 during scope.
    BEZEL_EXPECT_EQ(run(k894DictFieldReassign), 0);
}

// ---- Value-typed V: now rejected at instantiation per stdlib §4.2/#1327 ----

BEZEL_TEST(codegen_owned_dict_field_893, value_v_rejected_at_sema_1327) {
    // stdlib §4.2/#1327: OwningDictionary<string, int> is rejected at sema
    // (int is a primitive value type with no disposal obligation).
    auto r = runJit(kValueVNoCrash);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---- Borrowing dict field (not owned): values NOT disposed on Box disposal ---

BEZEL_TEST(codegen_owned_dict_field_893, borrowing_dict_field_no_cascade) {
    // Non-owned dict field: values must NOT be disposed when Box is disposed.
    // Cell._n stays 2 while the outer using-var scope is still live.
    BEZEL_EXPECT_EQ(run(kBorrowingDictFieldNoCascade), 0);
}

// ---- Regression: OwningArray/OwningLinkedList fields still dispose elements ---

BEZEL_TEST(codegen_owned_dict_field_893, owned_array_field_regression) {
    // OwningArray<Cell> field must still dispose its elements → Cell._n = 0.
    BEZEL_EXPECT_EQ(run(kOwnedArrayFieldRegression), 0);
}

BEZEL_TEST(codegen_owned_dict_field_893, owned_linkedlist_field_regression) {
    // OwningLinkedList<Cell> field must still dispose its elements → Cell._n = 0.
    BEZEL_EXPECT_EQ(run(kOwnedLLFieldRegression), 0);
}

// ---- Regression: #824 OwningArray field store still disposes old elements ----

BEZEL_TEST(codegen_owned_dict_field_894, owned_array_field_reassign_regression) {
    // OwningArray field reassign still disposes old elements → 0 at end.
    BEZEL_EXPECT_EQ(run(k824OwnedArrayReassignRegression), 0);
}
