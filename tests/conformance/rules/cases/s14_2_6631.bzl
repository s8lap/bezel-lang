//! case: s14_2_6631
//! doc: spec
//! section: 14.2
//! title: two interface instantiations with non-equivalent sets leave no inherited set
//! rule: a method that implements or overrides two or more generic declarations (two interfaces, two instantiations of one interface, or a base virtual member and an interface) whose constraint sets are not equivalent has no inherited set: it is a compile error at the implementing type, naming both sets, whether the method's clauses are omitted or restated
//! cites: 14.2
//! expect: reject
//! diagnostic: equivalent
//! issue: 8684
module Conf;
class Num
  public constructor()
  end constructor;
end Num;
class Other
  public constructor()
  end constructor;
end Other;
interface IMark
  method Mark(): int;
end IMark;
interface IBox<T>
  method Tag(): int;
end IBox;
class NB: IBox<Num>
  public constructor()
  end constructor;
  public method Tag(): int => 3;
end NB;
class NBM: IBox<Num>, IMark
  public constructor()
  end constructor;
  public method Tag(): int => 3;
  public method Mark(): int => 4;
end NBM;
class OB: IBox<Other>
  public constructor()
  end constructor;
  public method Tag(): int => 5;
end OB;
interface IQ<T>
  method Z<S>(a: S): int where S: IBox<T>;
end IQ;
class D: IQ<Num>, IQ<Other>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IBox<Num> => 0;
end D;
class P
  public static method Main(): int

    Console.WriteLine(1);
    return 0;
  end Main;
end P;
