// fix #4222 (§26.3 "Restrictions" / Appendix A.4, v0.298).
//
// §26.3's first bullet is exhaustive: "Only methods and computed read-only
// properties." Appendix A.4 states the same boundary structurally:
//   Member          ::= ... | PolymorphicMember | ...
//   ExtensionMember ::= MethodDecl | ExpressionPropertyDecl
// `PolymorphicMember` (`polymorphic("disc": Type, ...);`) is NOT one of the
// two `ExtensionMember` alternatives, and has no AST `Member` subclass at
// all — A.4's `PolymorphicMember` clause mutates the enclosing `TypeDecl`'s
// `serializedClass` field directly (`Parser::parsePolymorphicMember`,
// src/parser/Parser_Decls.cpp), reachable only from `parseTypeDeclBody`'s
// class/record member loop. So `polymorphic(...)` inside an extension body
// was never a SILENT admission (this issue is diagnostic-quality-only, not
// a soundness gap): `Parser::parseExtensionDecl`'s member loop called the
// general `parseMember()`, which has no arm for a bare `polymorphic`
// identifier followed by `(`, so the clause fell through to ordinary
// member-kind dispatch and cascaded ONE "expected member visibility ...
// or member-kind keyword" diagnostic PER STRAY TOKEN, none naming §26.3.
//
// MEASURED against r18 base `6f46a73` (this run, #4213/#4204/#4228
// applied, pre-#4222): this exact program produced SEVEN diagnostics, one
// per stray token consumed as garbage member-kind-dispatch input, none
// naming §26.3 — this test's own file IS the issue's measured repro.
//
// THE FIX: `parseExtensionDecl`'s member loop peeks for `polymorphic`
// immediately followed by `(` (mirroring `parseTypeDeclBody`'s existing
// class/record special-case peek) and parses-and-discards the clause with
// a dedicated `parseAndDiscardExtensionPolymorphicMember`, emitting exactly
// ONE diagnostic naming §26.3 instead of calling `parsePolymorphicMember`
// (which mutates a `TypeDecl` an extension body does not have).

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc -c %s 2>&1 | wc -l | FileCheck %s --check-prefix=COUNT

module M;
class WidgetP4222
  public X: int;
end WidgetP4222;
class WidgetP4222b
  public X: int;
end WidgetP4222b;
extension WidgetP4222
  polymorphic("a": WidgetP4222b);
end extension;
class CC4222 public static method Main(): int return 0; end Main; end CC4222;

// CHECK: {{.*}}:{{[0-9]+}}:{{[0-9]+}}: an extension block may not declare a `polymorphic` member registry
// CHECK-SAME: §26.3

// COUNT: {{^1$}}
