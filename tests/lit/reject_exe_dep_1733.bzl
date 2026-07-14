// Build the helper (module A, with a qualifying Main) as an EXECUTABLE artifact,
// then present that executable as a dependency of this module — a build error
// per §2.8.4 rule 4. Then build the SAME helper as a LIBRARY (`-c`) and import
// it successfully: the reject keys on the artifact's KIND, not its interface.
//
// RUN: %bezelc -o %t.exe %S/reject_exe_dep_1733_helper.bzl
// RUN: %not %bezelc -c %s --import %t.exe 2>&1 | FileCheck %s
// RUN: %bezelc -c -o %t.lib.o %S/reject_exe_dep_1733_helper.bzl
// RUN: %bezelc -c -o %t.b.o %s --import %t.lib.o
//
// (`%not` = LLVM's exit-status inverter; the middle compile is expected to fail
// — see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.
// `%S` = this test file's directory, so line 1 builds the helper's executable
// artifact into scratch `%t.exe`, exactly the shape the issue supplies.)
//
// Issue #1733 — the compiler must reject an EXECUTABLE artifact presented as a
// dependency (spec §2.8.4 rule 4, v0.188).
//
// Spec §2.8.4 rule 4: "Dependency artifacts are library-kind. Presenting an
// executable artifact as a dependency is a build error naming the artifact and
// its kind. (An executable carries a native entry symbol; admitting it as a
// dependency re-opens rule 3's perturbation hole from the other side …)."
//
// Before this fix the metadata carried NO artifact-kind, so an imported
// executable was accepted at load (its symbols imported) and the failure
// surfaced only as a raw linker collision — `multiple definition of 'main' /
// '_start'` — with no §2.8.4 diagnostic at all (issue #1511's `-c` fix covered
// only the entry-point lowering, not the dependency-load check). Schema v6 adds
// an artifact-kind flag (executable vs library) that module::loadDependency
// checks: an executable dependency is now rejected here, before codegen.
module B;
import A;

public class Program
  public static method Main(): int
    return Greeter.Greet();
  end Main;
end Program;

// bezelc reports this as a tool-level `bezelc: error: …` line (no source
// location — it is an artifact-set error, not a source diagnostic). Match the
// artifact-naming, the kind word, and the rule citation.
// CHECK: dependency artifact {{.*}} is an executable, not a library (§2.8.4 rule 4)
