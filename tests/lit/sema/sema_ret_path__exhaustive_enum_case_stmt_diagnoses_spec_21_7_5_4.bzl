// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2129 (§21.7 / §5.4): the choice case-statement totality grant is deliberately
// NOT extended to ENUMS. An enum's value space is its underlying integer type,
// and a non-constant cast `E(n)` produces a "raw" enum value equal to no
// declared constant (§5.4). A case covering only the declared constants
// therefore does NOT cover every runtime value: a raw value matches no arm and,
// in STATEMENT position, falls through with no action (§19.6), reaching the
// method end with no return. Accepting such a case as total would be UNSOUND, so
// §21.7 must still require a trailing return even though every declared constant
// is covered and every arm returns.
//
// Cite spec §21.7, §19.6, §5.4, §10.3 (v0.217).

module M;
enum E Red, Green, Blue; end E;
class C
  public method Classify(e: E): int
    case e of
      Red   => return 0;,
      Green => return 1;,
      Blue  => return 2;
    end;
  end Classify;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Classify` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
