//! case: s14_2_4909
//! doc: spec
//! section: 14.2
//! title: an override two levels below a generic base omitting where rejects an argument outside the bound
//! rule: Omitting the `where` clauses is the recommended spelling and means exactly the inherited set ... dispatch flows through the base or interface signature, so its constraints are the only ones call sites are ever checked against
//! cites: 14.1, 11.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
//! issue: 8649
module Conf;
interface IBox<T>
  static method Make(): owned T;
end IBox;
class Num: IBox<Num>
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  public static method Make(): owned Num => new Num(3);
end Num;
class Num2: IBox<Num2>
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  public static method Make(): owned Num2 => new Num2(4);
end Num2;
class B<X>
  public virtual method Z<S>(a: S): int where S: IBox<X> => 1;
end B;
class D<Y>: B<Y>
  public override method Z<S>(a: S): int => 2;
end D;
class E: D<Num>
  public constructor()
  end constructor;
  public override method Z<S>(a: S): int => 3;
end E;
class P
  public static method Main(): int
    var e := new E();
    Console.WriteLine(e.Z(new Num2(1)));
    return 0;
  end Main;
end P;
