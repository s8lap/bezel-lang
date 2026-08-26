// RUN: %bezelc -c -o %t.dep.o %S/self_import_1730_dup_helper.bzl
// RUN: %not %bezelc -c %s --import %t.dep.o 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md. `%S` = this test file's directory, so the first RUN
// line builds self_import_1730_dup_helper.bzl's `module Dup;` into a scratch
// `%t.dep.o` artifact, exactly like the `dup1.o` the issue supplies.)
//
// Issue #1730 shape 2 — self-import must be a compile error EVEN WHEN a
// dependency artifact declaring the same module name is supplied (spec
// §2.8.1 rule 4, v0.188).
//
// Before #1730, `module Dup; import Dup;` built against a `dup1.o` that
// also declares `module Dup;` did NOT reject: the artifact's publics became
// visible as if the import were an ordinary cross-module import (internals
// stayed unresolved) — a third, nowhere-specified semantics (neither the
// spec's literal same-file-is-a-noop reading nor rejection; see proposal
// 0034 §1). Rule 4 makes self-import an unconditional reject: it does not
// matter whether an artifact of the same name happens to be on the build's
// dependency list — the file's OWN `import Dup;` still names its own module.
//
// #1729 (v0.188) added §2.8.1 rule 3's "dep declaring a name the root also
// declares" rejection, which fires BEFORE sema's self-import check (the
// loader's artifact-set validation runs before type resolution). The build
// still fails — exactly the §2.8.1 rule 3 build error naming the dep and
// the root's module name — and rule 4's self-import rejection is foreclosed
// by the earlier rule-3 failure (the file's `import Dup;` is moot when the
// dep is rejected for the more general rule-3 reason). The diagnostic below
// is the loader's; rule 4 still fires in any self-import that triggers
// without a dep-of-same-name (see the other self-import_* lit tests).
module Dup;
import Dup;

class Program
  public static method Main(): int return 0; end Main;
end Program;

// CHECK: bezelc: error: dependency artifact '{{.+}}' declares module 'Dup', which is also the name of the root compilation; a build's dependency closure cannot redeclare a module the root already supplies (§2.8.1 rule 3)
