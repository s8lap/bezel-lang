// fix #5285 — spec §14.1 "Instantiation and its site" (v0.313): a constructed
// type "in a type alias (§27.8)" is an instantiation site, and a rule firing at
// the instantiation site "fires at every such site".  `A<int>` forms `G<int>`
// at TWO sites — the alias declaration's own body and the field naming the
// alias — so exactly two diagnostics.  The count lives here rather than in a
// gtest because the in-process runAot sink folds same-text diagnostics (#5266).
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
type A<T> := G<T>;
class H
  private _a: A<int>?;
end H;
class C
  public static method Main(): int
    return 0;
  end Main;
end C;
