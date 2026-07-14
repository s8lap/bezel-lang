// RUN: %bezelc -c -o %t.dep.o %S/send_1732_imported_class_sendability_lib.bzl
// RUN: %not %bezelc -c %s --import %t.dep.o 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the importer compile is expected to
// FAIL — see tests/lit/README.md. The first RUN line builds the SendLib
// library into a scratch `%t.dep.o` artifact, which the second RUN line then
// imports via `--import`.)
//
// Issue #1732 — a cross-module-IMPORTED class must have its §30.14 sendability
// judged from its ACTUAL reconstructed members, not vacuously defaulted to
// sendable. Spec §2.8.2 clause 1 / §30.14, v0.196.
//
// Before this fix, ExprTC::isSendable's Class branch bailed out with `return
// true` (sendable) whenever the TypeSymbol had no ast::TypeDecl — which is
// exactly the case for an imported class (ModuleLoader reconstructs its members
// as sema::FieldSymbol/PropertySymbol with no AST decl). So an imported
// NON-sendable class silently satisfied the §30.15 `Channel<T>` element rule
// and could cross a domain boundary — a data race. The fix walks the
// reconstructed member list (type via ctx_.importedMemberType, `owned` via
// MemberSymbol::modifiers), applying the same per-field Rule 2/3 check.
//
// `Tree` (imported, non-owned class-typed field) and `OwnsNonSendable`
// (imported, `owned` field of the non-sendable `Tree`) are both non-sendable;
// each `Channel<T>` element position must therefore be rejected.
module App;
import SendLib;

class C
  public static method Main(): int
    using var a := new Channel<Tree>();
    using var b := new Channel<OwnsNonSendable>();
    return 0;
  end Main;
end C;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape and the rule text/citation.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Tree` of `Channel<T>` is not sendable
// CHECK-SAME: (§30.14/§30.15, #600)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `OwnsNonSendable` of `Channel<T>` is not sendable
// CHECK-SAME: (§30.14/§30.15, #600)
