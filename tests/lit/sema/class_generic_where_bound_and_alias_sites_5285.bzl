// fix #5285 — spec §14.1 "Instantiation and its site" (v0.313): a constructed
// type "in a `where` bound" and "in a type alias (§27.8)" is an instantiation
// site, and §14.2's constraint checks "fire at every such site" (§3.6 rule 2
// supplies the rule; §12.7 "Value-typed operands" the rejection).  #5266
// recorded the annotation positions; these two are the remaining unrecorded
// ones, so `G<int>` in either was wrongly ACCEPTED while the same `G<int>` in
// a field annotation rejects.  Two sites here — the class's `where` bound and
// the alias declaration — so exactly two diagnostics.
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
class H<U> where U: G<int>
end H;
type A := G<int>;
class C
  public static method Main(): int
    return 0;
  end Main;
end C;
