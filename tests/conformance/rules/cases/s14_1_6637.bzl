//! case: s14_1_6637
//! doc: spec
//! section: 14.1
//! title: `this` receiver in a class deriving from `D<Num>`: non-conforming candidate rejected
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
class E: D<Num>
  public method Go(): int => this.Z<Num2>(new Num2());
end E;
class P
  public static method Main(): int
    var e := new E();
    Console.WriteLine(e.Go());
    return 0;
  end Main;
end P;
