// RUN: %bezelc -c -o %t.o %s
//
// Helper library artifact for the send_1732_imported_class_sendability_{reject,
// accept}.bzl sibling tests (issue #1732 — an imported class must have its
// §30.14 sendability judged from its ACTUAL reconstructed members, not
// vacuously defaulted to sendable). This module declares two non-sendable and
// two sendable sealed classes; the sibling tests import THIS module's artifact
// (via `--import`) and try to carry each type across a domain boundary (a
// `Channel<T>` element must be sendable, §30.14/§30.15).
//
// This file's own RUN line is an unremarkable §2.4 accept check (the library
// compiles clean); its role as a dependency artifact is what the siblings
// exercise. Cite spec §30.14 / §2.8.2, v0.196.
module SendLib;

// Non-sendable: a non-owned class-typed field is a borrow alias — §30.14
// Rule 2 (class-typed fields must be `owned`).
public sealed class Node
  public Val: int := 0;
  destructor end destructor;
end Node;
public sealed class Tree
  public Parent: Node?;
  public Val: int := 0;
  destructor end destructor;
end Tree;

// Sendable: only value-typed fields.
public sealed class Point
  public X: int := 0;
  public Y: int := 0;
  destructor end destructor;
end Point;

// Sendable: an `owned` field of a sendable class is transfer-closed (§30.14
// Rule 2 — owned AND the class is sendable). Exercises the `owned` modifier
// being read from the RECONSTRUCTED member on the imported path.
public sealed class OwnsSendable
  public owned Kid: Point := new Point();
end OwnsSendable;

// Non-sendable: an `owned` field of a NON-sendable class fails the recursive
// §30.14 Rule 2 check (the field is owned, but its class Tree is not sendable).
public sealed class OwnsNonSendable
  public owned Bad: Tree := new Tree();
end OwnsNonSendable;
