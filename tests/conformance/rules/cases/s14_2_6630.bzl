//! case: s14_2_6630
//! doc: spec
//! section: 14.2
//! title: the example restated with a different bound is a differing clause
//! rule: A **differing** clause — added, removed, narrowed, or widened relative to the inherited set — is a compile error naming the inherited constraints
//! cites: 14.2
//! expect: reject
//! diagnostic: clause
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
interface IFoo
  method Hi(): int;
end IFoo;
interface I<U>
  method M<S>(x: S): int where S: U;
end I;
class C: I<IFoo>
  public constructor()
  end constructor;
  public method M<T>(x: T): int where T: IMark => 0;
end C;
class P
  public static method Main(): int

    Console.WriteLine(1);
    return 0;
  end Main;
end P;
