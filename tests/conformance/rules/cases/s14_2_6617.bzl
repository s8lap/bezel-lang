//! case: s14_2_6617
//! doc: spec
//! section: 14.2
//! title: a restated bound nested in another constructed type is read under the substitution
//! rule: the inherited set is read under the implemented interface's or overridden base's substitution, its method type parameters renamed positionally
//! cites: 14.2
//! expect: run
//! output: 3
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

class NBB: IBox<IBox<Num>>
  public constructor()
  end constructor;
  public method Tag(): int => 3;
end NBB;
interface IQ<T>
  method Z<S>(a: S): int where S: IBox<IBox<T>>;
end IQ;
class D: IQ<Num>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IBox<IBox<Num>> => a.Tag();
end D;
class P
  public static method Main(): int

    var d := new D();
    Console.WriteLine(d.Z(new NBB()));
    return 0;
  end Main;
end P;
