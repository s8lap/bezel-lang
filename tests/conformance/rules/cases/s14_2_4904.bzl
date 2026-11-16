//! case: s14_2_4904
//! doc: spec
//! section: 14.2
//! title: a generic-class override restating where S: IBox<Y> rejects a class not conforming to IBox<Num>
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it, records and enums implementing it, interfaces extending it
//! cites: 14.1
//! expect: reject
//! diagnostic: constraint
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
class Other
  public constructor()
  end constructor;
end Other;
class B<X>
  public virtual method Z<S>(a: S): owned X where S: IBox<X> => S.Make();
end B;
class D<Y>: B<Y>
  public constructor()
  end constructor;
  public override method Z<S>(a: S): owned Y where S: IBox<Y> => S.Make();
end D;
class P
  public static method Main(): int
    var d := new D<Num>();
    using var n := d.Z<Other>(new Other());
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
