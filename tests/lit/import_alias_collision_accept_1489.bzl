// RUN: %bezelc -c -o %t.liba.o %S/import_alias_collision_1489_liba.bzl
// RUN: %bezelc -c -o %t.libb.o %S/import_alias_collision_1489_libb.bzl
// RUN: %bezelc -c -o %t.o %s --import %t.liba.o --import %t.libb.o
//
// Issue #1489 accept companion (spec §2.2, v0.183, #1515). Same two imports as
// the reject sibling, but the alias name `Ty` collides with NO imported bare
// name (LibA exports `Tx`, LibB exports `Ledger`), so the §2.2 alias-collision
// rule does not fire and the alias is a legal additional qualifier. The alias
// is even used (`Ty.Ledger.Get()`) to confirm ordinary §2.7 alias resolution
// is unaffected. A zero exit is the pass — the fix must not over-reject a
// non-colliding alias.
module App;
import LibA;
import LibB as Ty;

public class Program
  public static method Main(): int
    return Ty.Ledger.Get();
  end Main;
end Program;
