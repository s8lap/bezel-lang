//! case: s14_2_6625
//! doc: spec
//! section: 14.2
//! title: a class implementing `IQ<IBox<U>>` that restates `IBox<U>` has a differing clause
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
interface IQ<T>
  method Z<S>(a: S): int where S: IBox<T>;
end IQ;
class G<U>: IQ<IBox<U>>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IBox<U> => 0;
end G<U>;
class P
  public static method Main(): int

    Console.WriteLine(1);
    return 0;
  end Main;
end P;
