// RUN: %bezelc -c -o %t.o %s
//
// Helper library artifact for the send_1824_imported_record_sendability_{reject,
// accept}.bzl sibling tests (issue #1824 — the RECORD sibling of #1732: an
// imported record must have its §30.14 sendability judged from its ACTUAL
// reconstructed members, not vacuously defaulted to sendable). This module
// declares borrow-bearing (non-sendable) and all-value (sendable) records; the
// sibling tests import THIS module's artifact (via `--import`) and try to carry
// each record across a domain boundary (a `Channel<T>` element must be
// sendable, §30.14/§30.15).
//
// This file's own RUN line is an unremarkable §2.4 accept check (the library
// compiles clean); its role as a dependency artifact is what the siblings
// exercise. Cite spec §30.14 / §2.8.2, v0.196.
module RecLib;

public sealed class Node
  public Val: int := 0;
  destructor end destructor;
end Node;

// Non-sendable: a non-owned class-typed auto-property is a borrow alias, so the
// record is borrow-bearing (§9.7) and NOT sendable (§30.14 value-type rule).
// Body-member (auto-property) path.
public record BorrowArg
  public Ref: Node?;
end BorrowArg;

// Non-sendable: same borrow, but via the POSITIONAL field path (a positional
// record field reconstructs as a FieldSymbol on the imported side — exercises
// that arm of the reconstructed-member walk).
public record PosBorrow(Ref: Node?);

// Non-sendable: a field whose type is the non-sendable BorrowArg record — the
// per-field check recurses into the nested record (§30.14).
public record NestBorrow
  public Inner: BorrowArg;
end NestBorrow;

// Sendable: only value-typed fields.
public record ValArg
  public X: int;
  public Y: int;
end ValArg;

// Sendable: a field whose type is the sendable ValArg record — the recursion
// bottoms out sendable, so NestVal is sendable (over-reject guard).
public record NestVal
  public Inner: ValArg;
end NestVal;
