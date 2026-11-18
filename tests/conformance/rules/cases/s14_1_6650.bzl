//! case: s14_1_6650
//! doc: spec
//! section: 14.1
//! title: nested receiver `D<D<Num>>`, bound `IBox<Y>`: non-conforming candidate rejected
//! rule: An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 14.2, 12.9
//! expect: reject
//! diagnostic: type argument
//! issue: 8663
module Conf;
interface IBox<T>
  method Tag(): int;
end IBox;
class Num: IBox<Num>
  public constructor()
  end constructor;
  public method Tag(): int => 3;
end Num;
class Num2: IBox<Num2>
  public constructor()
  end constructor;
  public method Tag(): int => 4;
end Num2;
class D<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IBox<Y> => a.Tag();
  public method ZL<S>(a: S): int where S: IBox<D<Y>> => a.Tag();
end D;
class WN: IBox<D<Num>>
  public constructor()
  end constructor;
  public method Tag(): int => 9;
end WN;
class WN2: IBox<D<D<Num>>>
  public constructor()
  end constructor;
  public method Tag(): int => 8;
end WN2;
class P
  public static method Main(): int
    var d := new D<D<Num>>();
    Console.WriteLine(d.Z<Num2>(new Num2()));
    return 0;
  end Main;
end P;
