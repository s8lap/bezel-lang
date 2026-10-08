// fix #1970 / §28.4 / §28.6: an owned local transferred via `owned x` to one of
// the EXCLUDED owning-collection shapes (OwningSet dedup-insert / OwningLinkedList
// / OwningDictionary / manual-lowered builtin ctor Shared<T>) is a taint that,
// on a throw that PRECEDES the transfer, leaks the still-live local — a residual
// of #1955 (the #1955 pre-gate review deliberately left the walker INELIGIBLE for
// these shapes because their lowering did not null the caller's slot, and
// broad-untainting would have been a double-free).
//
// #1970 closes the residual: each excluded shape's transfer lowering now NULLS
// the caller's slot for a bare owned local / owned parameter (mirroring the
// #1955 allowlisted-collection + ctor path), so the walker can safely untaint
// the source and the null-guarded unwindOnly disposal discharges the value on
// the throw that PRECEDES the transfer. A throw AFTER the transfer still leaves
// the slot null (the caller's disposal skips it) while the recipient owns the
// value — no double-free, no leak.
//
// The acceptance bar:
//   FAIL-WITHOUT: ASan/LSan reports the leak (r.ran==false, value > 10).
//   PASS-WITH:    exit == 10 (caught == 1 AND Resource._live == 0).
//
// Every leg rides runAotChecked (AOT + ASan/LSan oracle); the destructor-count
// exit code is asserted together with the ASan-clean r.ran flag.
//
// Spec citation: §28.4 (unwind disposal) + §28.6 (dispose-on-throw), v0.197.
// Pair with the #1970 codegen fix in src/codegen/LlvmBackend_Assign.cpp +
// src/codegen/LlvmBackend_Calls.cpp + src/codegen/LlvmBackend_Calls3.cpp +
// src/codegen/LlvmBackend_Exprs2.cpp + src/codegen/LlvmBackend_Setup.cpp.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Resource: static net-live counter. Disposed exactly once => net 0; leaked =>
// +1; double-disposed => -1 (and the second reclaim trips ASan under the check).
const char* kResource =
    "class Resource "
    "  public static _live: int := 0; "
    "  public constructor() Resource._live := Resource._live + 1; end constructor; "
    "  destructor Resource._live := Resource._live - 1; end destructor; "
    "end Resource; ";

}  // namespace

// ===========================================================================
// #1970 CORE — OwningSet<T>.Add(owned x) (the #1474 dedup-insert path). A throw
// BEFORE the Add leaves `b` live => must be unwind-disposed exactly once.
// FAIL-WITHOUT: Resource._live == 1 (leak).  PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owningset_add_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var s := new OwningSet<Resource>(); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live (§28.4)
        "    s.Add(move b); "                  // OwningSet.Add dedup-inserts (stdlib §4.10)
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // ASan/LSan-clean (no leak, no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningSet<T>.Add transfer-then-throw double-free guard. A throw AFTER
// the Add: OwningSet owns `b` (or disposed it on the dup branch) and the
// caller's nulled slot must skip it.
// FAIL-WITHOUT (the broad-null regression): ASan trips (r.ran==false).
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owningset_add_transfer_then_throw_no_double_free_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var s := new OwningSet<Resource>(); "
        "    var b := new Resource(); "
        "    s.Add(move b); "                  // b moved into s; slot nulled
        "    Boom(); "                          // throw AFTER transfer
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningLinkedList<T>.AddLast(owned x) (linked-list runtime path, not
// array-backed). A throw BEFORE the AddLast leaves `b` live => must be
// unwind-disposed exactly once.
// FAIL-WITHOUT: leak (value 11).  PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owninglinkedlist_addlast_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var ll := new OwningLinkedList<Resource>(); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    ll.AddLast(move b); "             // OwningLinkedList.AddLast (linked-list runtime)
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningLinkedList<T>.AddLast transfer-then-throw double-free guard.
// The list (a `using var`) disposes `b` on unwind; the caller's nulled slot
// must skip it.  FAIL-WITHOUT (the broad-null regression): ASan trips.
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owninglinkedlist_addlast_transfer_then_throw_no_double_free_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var ll := new OwningLinkedList<Resource>(); "
        "    var b := new Resource(); "
        "    ll.AddLast(move b); "
        "    Boom(); "                          // throw AFTER transfer
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningLinkedList<T>.AddFirst(owned x): head-prepend path. A throw
// BEFORE the AddFirst leaves `b` live => must be unwind-disposed exactly once.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owninglinkedlist_addfirst_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var ll := new OwningLinkedList<Resource>(); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    ll.AddFirst(move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningLinkedList<T>.AddFirst transfer-then-throw double-free guard.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owninglinkedlist_addfirst_transfer_then_throw_no_double_free_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var ll := new OwningLinkedList<Resource>(); "
        "    var b := new Resource(); "
        "    ll.AddFirst(move b); "
        "    Boom(); "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningLinkedList<T>.AddBefore(node, owned x) (mid-list insert).
// A throw BEFORE the AddBefore leaves `b` live => must be unwind-disposed
// exactly once. (The `node` argument is a borrow — a pre-existing node in
// the list — and is not part of the owned-transfer path.)
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owninglinkedlist_addbefore_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var ll := new OwningLinkedList<Resource>(); "
        "    ll.AddLast(new Resource()); "
        "    var node := ll.LastNode; "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    ll.AddBefore(node, move b); "
        "    _ := node; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningLinkedList<T>.AddAfter(node, owned x) (mid-list insert).
// A throw BEFORE the AddAfter leaves `b` live => must be unwind-disposed
// exactly once.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owninglinkedlist_addafter_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var ll := new OwningLinkedList<Resource>(); "
        "    ll.AddLast(new Resource()); "
        "    var node := ll.FirstNode; "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    ll.AddAfter(node, move b); "
        "    _ := node; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningDictionary<K,V>.Add(k, owned v) (§4.9 / §28.3 the #1214 path).
// A throw BEFORE the Add leaves `b` live => must be unwind-disposed exactly
// once.  FAIL-WITHOUT: leak.  PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owningdictionary_add_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var d := new OwningDictionary<string, Resource>(); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    d.Add(\"k\", move b); "           // OwningDictionary takes ownership
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningDictionary<K,V> indexer-set path `d["k"] := owned v` is the
// overwriting form (no ArgumentException; #1214 fix). A throw BEFORE the
// indexer-set leaves `b` live => must be unwind-disposed exactly once.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owningdictionary_indexerset_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var d := new OwningDictionary<string, Resource>(); "
        "    d.Add(\"k\", new Resource()); "  // pre-seed so indexer-set overwrites
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    d[\"k\"] := owned b; "              // overwriting indexer-set
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningDictionary<K,V>.Add transfer-then-throw double-free guard.
// The dictionary (a `using var`) disposes `b` on unwind; the caller's nulled
// slot must skip it.  FAIL-WITHOUT (the broad-null regression): ASan trips.
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owningdictionary_add_transfer_then_throw_no_double_free_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var d := new OwningDictionary<string, Resource>(); "
        "    var b := new Resource(); "
        "    d.Add(\"k\", move b); "
        "    Boom(); "                          // throw AFTER transfer
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningDictionary<K,V> indexer-set transfer-then-throw double-free
// guard.  FAIL-WITHOUT: ASan trips.  PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owningdictionary_indexerset_transfer_then_throw_no_double_free_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var d := new OwningDictionary<string, Resource>(); "
        "    d.Add(\"k\", new Resource()); "
        "    var b := new Resource(); "
        "    d[\"k\"] := owned b; "
        "    Boom(); "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — OwningArray<T>[i] := owned v (the #401 / §4.5 indexer-set path).
// A throw BEFORE the indexer-set leaves `b` live => must be unwind-disposed
// exactly once.  FAIL-WITHOUT: leak.  PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, owningarray_indexerset_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var arr := new OwningArray<Resource>(); "
        "    arr.Add(new Resource()); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    arr[0] := owned b; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 — new Shared<T>(owned x) (the §28.13 builtin owned-consuming ctor).
// The #1955 pre-gate review flagged this as a soundness hazard: a broad untaint
// would let Shared dispose R1 on the throw-after-transfer edge AND let the
// caller's unwind disposal free it again.  The #1970 narrow untaint: the slot
// IS nulled (the caller's unwind disposal skips it), the walker's eligibility
// is gated on a paired nulling path, and a transfer-then-throw control test
// confirms no double-free.  The throw-BEFORE transfer leg is the leak #1970
// closes — a throw leaves the bare owned local live and it must be
// unwind-disposed exactly once.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, builtin_shared_ctor_before_throw_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    using var sh := new Shared<Resource>(move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1970 CONTROL — the #1955 allowlisted owning collection (OwningArray.Add)
// and the #1918 user-method transfer paths stay regression-clean.  Mirrors
// the test of the same shape in
// tests/codegen/unwind_owned_transfer_ctor_collection_reassign_1955_1956_test.cpp
// — included here to guarantee the #1970 broad-null extension doesn't
// regress the #1955 / #1918 path.
// ===========================================================================
BEZEL_TEST(excluded_owning_collection_throw_leak_1970, regression_control_1955_owningarray_add_28_4) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var log := new OwningArray<Resource>(); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer
        "    log.Add(move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}
