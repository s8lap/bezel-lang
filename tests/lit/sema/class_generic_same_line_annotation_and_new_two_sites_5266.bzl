// fix #5266 — spec §14.1 "Instantiation and its site" (v0.313): every
// reference that forms an instantiation is a site and the rule "fires at every
// such site"; §12.7 / §3.6 rule 2 supply the rule. `var g: G<int> := new G<int>();` is the annotation site and the `new` site.
// Exactly two diagnostics (one per site).
//
// RUN: %not %bezelc -c %s 2>&1 | grep -c "as. OPERAND against the reference target" | FileCheck %s
// CHECK: {{^2$}}
module M;
interface IS end IS;
class G<T>
  public method F(v: T): bool
    var o := v as IS;
    return o <> null;
  end F;
end G;
class C
  public static method Main(): int
    var g: G<int> := new G<int>();
    return 0;
  end Main;
end C;
