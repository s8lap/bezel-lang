// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #4677 (§9.7 / §15.3 v0.307): a POSITIONAL record's constructor
// parameter whose type is an owning inline array (`[N owned]T`) is rejected
// — records support only copy semantics, not move semantics, and declare no
// disposal path (§9.7). checkRecordRestrictions already rejected this shape
// for a BODY-record field (`record Wrapper { public Items: [2 owned]T; }`,
// see sema_record__owned_field_diagnoses.bzl's sibling coverage) via a
// direct `dynamic_cast<const ast::FieldDecl*>(fs->decl)` read, but a
// positional field's storage (`fs->posParam`) never satisfies that cast, so
// this exact positional form — the rule-corpus repro, s15_3_0002 — compiled
// with NO diagnostic. Mirrors record_owning_choice_field_2210's positional/
// body split for the sibling owning-choice rule, whose positional check
// already used the correct both-sources accessor.

module M;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Wrapper(Items: [2 owned]Connection);

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Wrapper` may not have an owning inline-array field `Items`
// CHECK-SAME: (§9.7)
