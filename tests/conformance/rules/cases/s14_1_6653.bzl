//! case: s14_1_6653
//! doc: spec
//! section: 14.1
//! title: `Cmp<int>` receiver, bound `IComparable<Y>`: conforming candidate admitted
//! rule: An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 14.2
//! expect: run
//! output: 5
//! issue: 8663
module Conf;
class Cmp<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IComparable<Y> => 5;
end Cmp;
class P
  public static method Main(): int
    var c := new Cmp<int>();
    Console.WriteLine(c.Z(3));
    return 0;
  end Main;
end P;
