// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / stdlib §4.14 / §4.15 (v0.226, issue #1251): the builtin-SPAN face of
// the §9.7 instance-field storage rule. §9.7 reads "A borrow-bearing record
// value may not be stored in an **instance field of a class** … a
// record-carried borrow … may be consumed through access paths with **no
// per-access disposed-flag check** (a span's buffer window is the canonical
// case) … the store is **rejected statically**." The stdlib declares
// `Span<T>` (§4.14) and `ArraySpan<T>` (§4.15) as records over a class-typed
// backing store, so a span is a borrow-bearing record by §9.7's own
// definition — indeed the bullet names it as the canonical case.
//
// Before the fix, `MethodOwnership::checkBorrowStoreEscape`'s `Dest::Field`
// leg consulted only `isBorrowBearingRecord`, which requires a user Record
// `TypeDecl`; a builtin span has none, so a BARE span RHS matched nothing and
// the store compiled clean — `box.f := buf.AsSpan()` here left `Box.f`
// viewing a buffer disposed at the end of `Fill`, and the caller's `b.Read()`
// read the freed buffer (verified: rc=0 at compile time, ASan SEGV in
// `bz_array_get` at run time).

module M; class Box public f: Span<int>; public constructor() end constructor; public method Read(): int return f[0]; end Read; end Box; class C static method Fill(box: Box) using var buf := new OwningBuffer<int>(4); buf[0] := 42; box.f := buf.AsSpan(); end Fill; public static method Main(): int using var b := new Box(); Fill(b); return b.Read(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a span may not be stored in an instance field of a class
// CHECK-SAME: a span (§4.14/§4.15) is a borrow-bearing record over its backing Array / OwningBuffer
// CHECK-SAME: (§9.7; store the backing store instead
// CHECK-SAME: re-derive the span with `AsSpan()` at the use site)
