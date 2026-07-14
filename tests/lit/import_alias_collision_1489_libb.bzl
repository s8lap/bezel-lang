// RUN: %bezelc -c -o %t.o %s
//
// Helper artifact for import_alias_collision_1489.bzl and
// import_alias_collision_accept_1489.bzl (issue #1489 / spec §2.2, v0.183).
// Declares `module LibB;` exporting a public `Ledger`. A sibling test aliases
// this module (`import LibB as Tx;` / `as Ty;`); the alias name — not any name
// LibB exports — is what may or may not collide with LibA's bare `Tx`.
module LibB;

public class Ledger
  public static method Get(): int => 3;
end Ledger;
