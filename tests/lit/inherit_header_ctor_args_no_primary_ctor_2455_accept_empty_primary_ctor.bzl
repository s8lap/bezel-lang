// RUN: %bezelc -c -o %t.o %s
//
// fix #2455 (§11.1 / §22.10 / §A.3, v0.235): the CORRECT alternative
// spelling the reject diagnostic recommends — an EMPTY primary-constructor
// parameter list `()` on the derived class — legally threads a real base
// argument and must remain accepted. `PrimaryCtorParams ::= "(" ParamList?
// ")"` admits an empty `ParamList?`, so `Derived()` is a genuine (if
// trivial) primary constructor, distinct from writing no parens at all.

module M;

public class LocalBase
  private _v: int;
  public constructor(v: int) _v := v; end constructor;
  public property Val: int => _v;
end LocalBase;

public class LocalDerived(): LocalBase(99)
end LocalDerived;

public class C
  public static method Main(): int
    using var d := new LocalDerived();
    return d.Val;
  end Main;
end C;
