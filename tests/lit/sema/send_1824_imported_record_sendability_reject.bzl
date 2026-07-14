// RUN: %bezelc -c -o %t.dep.o %S/send_1824_imported_record_sendability_lib.bzl
// RUN: %not %bezelc -c %s --import %t.dep.o 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the importer compile is expected to
// FAIL — see tests/lit/README.md. The first RUN line builds the RecLib library
// into a scratch `%t.dep.o` artifact, which the second RUN line then imports
// via `--import`.)
//
// Issue #1824 — the RECORD sibling of #1732. A cross-module-IMPORTED record
// must have its §30.14 sendability judged from its ACTUAL reconstructed
// members, not vacuously defaulted to sendable. Spec §2.8.2 clause 1 / §30.14,
// v0.196.
//
// Before this fix, ExprTC::isSendable's Record branch bailed out with `return
// true` (sendable) whenever the record's TypeSymbol had no ast::TypeDecl —
// which is exactly the case for an imported record (ModuleLoader reconstructs
// its members as sema::FieldSymbol/PropertySymbol with no AST decl). So an
// imported borrow-bearing record silently satisfied the §30.15 `Channel<T>`
// element rule and could cross a domain boundary — a data race. The fix walks
// the reconstructed member list (type via ctx_.importedMemberType), applying
// the same per-field check as the AST path — positional fields, body fields,
// and auto-properties all covered.
//
// `BorrowArg` (imported, body auto-property of class type), `PosBorrow`
// (imported, positional field of class type), and `NestBorrow` (imported, field
// of the non-sendable `BorrowArg`) are each borrow-bearing → NOT sendable; each
// `Channel<T>` element position must therefore be rejected.
module App;
import RecLib;

class C
  public static method Main(): int
    using var a := new Channel<BorrowArg>();
    using var b := new Channel<PosBorrow>();
    using var c := new Channel<NestBorrow>();
    return 0;
  end Main;
end C;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape and the rule text/citation.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `BorrowArg` of `Channel<T>` is not sendable
// CHECK-SAME: (§30.14/§30.15, #600)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `PosBorrow` of `Channel<T>` is not sendable
// CHECK-SAME: (§30.14/§30.15, #600)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `NestBorrow` of `Channel<T>` is not sendable
// CHECK-SAME: (§30.14/§30.15, #600)
