// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §3.5 / §7.11, spec v0.220): `bool? += bool?` — the arithmetic
// operators are not defined on `bool` (§3.5), so there is no underlying
// operator for §7.11 to lift. Before the fix the nullable gate skipped the
// check and the #1964 lowering added the i1 payloads —
// `bool?(true) += bool?(false)` silently "computed" a boolean sum.
//
// fix #3830 (v0.288): `b`/`c` are PARAMETERS, not literal-initialized locals
// — a local declared `var b: bool? := true;` is §7.4-narrowed at its
// initializer, and #3830 makes narrowing reach compound assignment, so a
// narrowed `bool?` would no longer trip THIS gate at all (bool has no
// arithmetic operator regardless, §3.5, but the point of this file is the
// nullable-target diagnostic, not the bool one — a narrowed operand still
// hits the bool rejection, just without the `?` suffix in the message,
// which would drift this file's pinned wording for a reason unrelated to
// its subject). A parameter is never narrowed by what the caller passes —
// narrowing requires an in-body null test (§7.4) — so this keeps testing a
// genuinely nullable, unguarded operand exactly as originally intended.

module M;
class C
  public static method Chk(b: bool?, c: bool?): int b += c; return 0; end Chk;
  public static method Main(): int return Chk(true, false); end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not valid on a `bool?` target — the arithmetic operators are not defined on `bool` (§3.5); boolean combinations use `and`/`or`/`not` (§17.2)
