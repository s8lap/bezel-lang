// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 / Appendix C.4, v0.338 / #5805: a type bound naming
// `Object` (§8.1, the root type) is a compile error at the declaration; the
// remedy is an unconstrained `T` or `where T: notnull`. `Object` is a
// primitive-kind TypeRef, so the record/enum/choice declaration pass does not
// reach it; it was accepted silently before #5805 (rc=0, no diagnostic).
//
// Fails-without-fix: before #5805 this program compiled with rc=0 and no
// diagnostic. The class is never instantiated, so only the declaration-level
// check can catch it.

module M;

public class Keep<T> where T: Object
  public constructor()
  end constructor;
end Keep;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Object`, the root type
// CHECK-SAME: (§14.2)
