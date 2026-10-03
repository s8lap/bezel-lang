// RUN: %bezelc -c -o %t.o %s
//
// Helper dependency artifact for
// imported_literal_target_5328__owning_array_imported.bzl (issue #5328 /
// spec §15.4, v0.313). Its own trivial RUN line per the tests/lit convention;
// it asserts the EXPORTER compiles, without which the importer row would be
// vacuous.
//
// `Box` is a TRIVIAL-DISPOSE class (§28.9) on purpose: §15.4's literal-tree
// ownership rule admits a fresh element of trivial-dispose class type as a
// fresh node of the literal's tree, owned and freed by whatever owns the
// subtree's root. That is the ownership question issue #5328 item 4 turns on
// across a module boundary, and it is why that row is a typing control only.
module OAbLib5328;

public class Box
  private _v: int;
  public constructor(v: int) _v := v; end constructor;
  public method V(): int => _v;
end Box;

public class OabOps
  public static method OAb(xs: OwningArray<Box>): int => xs.Count;
end OabOps;
