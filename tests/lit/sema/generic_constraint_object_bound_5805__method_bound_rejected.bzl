// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.338 / Appendix C.4, v0.338 / #5805: the method-level form of
// the `Object`-bound rule. A method's own `where` clause naming `Object` is a
// compile error at the declaration, independent of any call site.
//
// Fails-without-fix: before #5805 this program compiled with rc=0 and no
// diagnostic (the method is never called).

module M;

public class Util
  public static method Count<T>(x: T): int where T: Object => 1;
end Util;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Object`, the root type
// CHECK-SAME: (§14.2)
