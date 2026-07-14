// RUN: %bezelc -c -o %t.liba.o %S/import_alias_collision_1489_liba.bzl
// RUN: %bezelc -c -o %t.libb.o %S/import_alias_collision_1489_libb.bzl
// RUN: %not %bezelc -c %s --import %t.liba.o --import %t.libb.o 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the final compile is expected to fail
// — see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.
// `%S` = this test file's directory, so the first two lines build the LibA /
// LibB dependency artifacts the third line imports.)
//
// Issue #1489 — an import alias that collides with an unambiguously imported
// public bare name must be a compile error at the alias declaration (spec
// §2.2, v0.183, #1515).
//
// Spec §2.2 (v0.183): "An alias may not collide with another alias, an
// imported module's name segment in qualifier position, a type declared in
// the file's module, or an unambiguously imported public bare name (v0.183,
// #1515): §2.7 rule 1 resolves imported bare names before aliases with no
// backtracking, so a same-named alias would be unreachable in every chain-head
// position — the collision is an error at the alias declaration rather than a
// silently dead alias."
//
// Here `import LibA;` makes LibA's public bare type `Tx` visible unambiguously
// (no other import exports `Tx`), and `import LibB as Tx;` then binds an alias
// with the same name — unreachable per §2.7 rule 1. Before this fix the
// compiler silently ACCEPTED the alias (issue #1489's probe: neither reading
// of §2.7 diagnosed it; the search merely backtracked). The diagnostic must
// fire at the alias declaration (line 3 below), not at any use site.
module App;
import LibA;
import LibB as Tx;

public class Program
  public static method Main(): int return 0; end Main;
end Program;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape and the rule text.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: import alias `Tx` collides with the unambiguously imported public bare name
// CHECK-SAME: (§2.2)
