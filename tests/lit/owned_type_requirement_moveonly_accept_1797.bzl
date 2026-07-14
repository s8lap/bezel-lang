// ACCEPT control for the MOVE-ONLY arm of issue #1797 (spec §28.1 "Type
// requirement", v0.193; §15.3 move-only value types).
//
// §28.1: "for a move-only value type (§15.3) the modifier is admitted-but-
// redundant (the type's own move semantics already transfer the obligation)."
// The single move-only value type is the owning inline array `[N owned]T` /
// `[N owned]T?` (§15.3). So `owned a: [3 owned]Conn` must be ACCEPTED by the
// §28.1 admissibility check — it must NOT be rejected as an inadmissible
// (non-owning) inline array. The #1797 fix distinguishes the two: a non-owning
// `[N]T` rejects the marker (see owned_type_requirement_reject_1797.bzl), a
// move-only `[N owned]T` admits it.
//
// This case is asserted with a TARGETED no-diagnostic check: the compile must
// exit zero and emit no `error` and specifically no §28.1 rejection. It does
// NOT use the blanket `§`-free assertion, because the ownership pass emits an
// incidental §28.3 "auto-drop" INFORMATIONAL warning for the (redundant)
// `owned` marker on a move-only parameter — a separate, pre-existing
// ownership-pass behavior orthogonal to §28.1's declaration-site admissibility
// (a move-only array's slots auto-dispose at scope end either way, §15.3).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class Conn
  public constructor() end constructor;
end Conn;
class R
  // owned MOVE-ONLY value type (owning inline array) — admitted-but-redundant.
  public method A5(owned a: [3 owned]Conn): () return; end A5;
end R;

// NODIAG-NOT: error
// NODIAG-NOT: §28.1
