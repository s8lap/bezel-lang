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
// Before this fix, `module Dup; import Dup;` built against a `dup1.o` that
// also declares `module Dup;` did NOT reject: the artifact's publics became
// visible as if the import were an ordinary cross-module import (internals
// stayed unresolved) — a third, nowhere-specified semantics (neither the
// spec's literal same-file-is-a-no-op reading nor rejection; see proposal
// 0034 §1). Rule 4 makes self-import an unconditional reject: it does not
// matter whether an artifact of the same name happens to be on the build's
// dependency list — the file's OWN `import Dup;` still names its own module.
module Dup;
import Dup;

class Program
  public static method Main(): int return 0; end Main;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: module `Dup` cannot import itself
// CHECK-SAME: (§2.8.1 rule 4)
