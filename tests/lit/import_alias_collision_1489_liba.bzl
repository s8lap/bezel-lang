// RUN: %bezelc -c -o %t.o %s
//
// Helper artifact for import_alias_collision_1489.bzl and
// import_alias_collision_accept_1489.bzl (issue #1489 / spec §2.2, v0.183).
// Declares `module LibA;` exporting a public bare type `Tx`, so a sibling
// test that also does `import LibB as Tx;` makes the alias `Tx` collide with
// this module's unambiguously imported public bare name `Tx`. Its own RUN
// line is an unremarkable library accept (§2.4); its role as a dependency
// artifact is what the sibling tests exercise.
module LibA;

public class Tx
  public static method Hello(): int => 7;
end Tx;
