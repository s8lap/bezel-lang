// REJECT test — the #1653 keyed-transit exception, FLIPPED for v0.213 (#2081).
//
// History: through spec v0.212 this was an ACCEPT control. Spec §28.6 (v0.184)
// carried a "transit exception" (axiom A8): an element borrow obtained from a
// BORROWING-family collection — an indexer-get, a default-iteration variable,
// dictionary-entry fields — MAY be disposed (the manual form of the also-retired
// `DisposeElements()`). v0.213 RETIRES axiom A8 (spec §28.6/§28.11, the A8
// tombstone; the #2075 corpus sweep found zero load-bearing clients): only an
// owner disposes, with no exception. `Dispose()` through any borrow — including
// a borrowing-family collection element — is now a compile error.
//
// The very source #1653 once accepted must therefore now be REJECTED. `arr` has
// static type `Array<Resource>` (a borrowing-family collection), so the default-
// iteration variable `e` and the bare indexer-get `arr[0]` are element borrows
// the caller does not own; both `e.Dispose()` and `arr[0].Dispose()` are errors.
// The sound replacements: declare the OWNING family and let `Dispose()` cascade,
// or take each element out first (`Drain()`/`Pop*`/`move coll[i]`) — exercised by
// the behavior test tests/codegen/dispose_transit_retired_2081_test.cpp.
//
// STRUCTURAL POINT (as dispose_borrow_receiver_1653.bzl): this rejection comes
// from the OWNERSHIP pass, which runs after sema; only the REAL `bezelc` (which
// lit drives) runs it, so the §28.6 diagnostic is always in the observed output.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
module M;
class Resource public method Ping(): () end Ping; end Resource;
class Program
  public static method Main(): int
    const arr: Array<Resource> := [new Resource()];
    arr.Add(new Resource());
    for var e in arr do
      e.Dispose();
    end;
    arr[0].Dispose();
    return 0;
  end Main;
end Program;

// Both element-borrow disposals are rejected by the ownership pass with the
// v0.213 A8-retirement diagnostic. bezelc emits `file:line:col: message` with no
// "error:" severity word, so we anchor on the `:line:col:` shape and the rule.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot call `Dispose()` on an element borrow read from a borrowing-family collection
// CHECK-SAME: §28.6
// CHECK-SAME: axiom A8
// The SECOND site (the bare indexer-get `arr[0].Dispose()`) must fire its own
// diagnostic — one report per violation, not one per function (#2081 review).
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot call `Dispose()` on an element borrow read from a borrowing-family collection
// CHECK-SAME: §28.6
