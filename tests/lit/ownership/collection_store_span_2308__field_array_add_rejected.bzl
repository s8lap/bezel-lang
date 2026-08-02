// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / stdlib §4.14 / §4.15 (v0.226, issue #2308): the
// "copied into a collection" bullet of §9.7's storage rule, the sibling of
// the #1251 instance-field fix. §9.7 reads: "A borrow-bearing record value
// may be **copied into a collection** only when every carried borrow's
// source provably outlives the collection's own binding scope." The
// compiler's `MethodOwnership::checkCollectionBorrowStoreEscape` consults
// `isBorrowBearingRecord` (and `isBorrowBearingChoice`, #2209) on the
// argument — both predicates require a user `TypeDecl`; the builtin spans
// (§4.14 `Span<T>` / `ReadOnlySpan<T>`, §4.15 `ArraySpan<T>` /
// `ReadOnlyArraySpan<T>`) have none, so a bare span element slipped the
// check. The store compiled clean and the runtime read a span whose
// backing buffer had been disposed (verified: rc=0 at compile, ASan
// heap-use-after-free in `bz_array_get` at run time).
//
// The fix extends the `RecvKind::Escapes` leg with the
// `isBorrowBearingRecordOrSpan` predicate and emits a span-flavoured
// diagnostic on the builtin-span branch (the null-source form has no
// corresponding `isNullSourceRecordExpr` match — a span's backing-store
// field is non-nullable per §4.14/§4.15 — so no exemption applies, in
// lockstep with the #1251 instance-field precedent).

module M; class Holder public owned spans: Array<Span<int>> := new Array<Span<int>>(); public constructor() end constructor; end Holder; class C static method Fill(h: Holder) using var buf := new OwningBuffer<int>(4); buf[0] := 77; h.spans.Add(buf.AsSpan()); end Fill; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a span may not be copied into a collection
// CHECK-SAME: (§9.7; store the backing store instead
// CHECK-SAME: re-derive the span with `AsSpan()` at the use site)
