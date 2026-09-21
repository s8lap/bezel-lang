// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #4565 (§10.3, spec v0.306): "The colon list is one list: if its first item
// resolves to one of the eight underlying-type keywords, that item is the
// underlying type; otherwise the underlying type is `int` and every item
// names an interface." `double` resolves to none of the eight, so it must be
// read as an attempted interface — and it is not one, so the program is
// rejected.
//
// Pre-fix, TypeResolution_Names.cpp's enum-underlying-type arm fired on
// `ref->kind == TypeRef::Kind::Primitive` alone — ANY primitive, at ANY
// colon-list position — so `double` was silently accepted as the underlying
// type and carried into codegen. Enum equality's `CreateICmpEQ` then
// unconditionally emitted `icmp eq double`, which LLVM's IR verifier rejects
// (measured pre-fix: "Invalid operand types for ICmp instruction" / "codegen
// error: IR verification failed (malformed IR emitted)", exit 1 — a crash-
// shaped failure with no diagnostic naming the rule, not this diagnostic).
//
// Per the spec's own ruling (Bezel_Changelog.md: "§10.3's colon-list fallback
// names the underlying-type fact"), the diagnostic below names the
// underlying-type fact rather than the literally-failing "not an interface".

module Conf;

enum Ratio: double
  Half, Whole;
end Ratio;
// CHECK: enum_colon_list_underlying_type_4565__double_head_rejected.bzl:[[@LINE-3]]:13: enum `Ratio`'s underlying type `double` is not one of the eight admissible underlying types
