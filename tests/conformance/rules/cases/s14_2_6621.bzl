//! case: s14_2_6621
//! doc: spec
//! section: 14.2
//! title: a restated bound over a nullable interface argument is read under that substitution
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

class NBN: IBox<Num?>
  public constructor()
  end constructor;
  public method Tag(): int => 3;
end NBN;
interface IQN<T>
  method Z<S>(a: S): int where S: IBox<T>;
end IQN;
class D: IQN<Num?>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IBox<Num?> => a.Tag();
end D;
class P
  public static method Main(): int

    var d := new D();
    Console.WriteLine(d.Z(new NBN()));
    return 0;
  end Main;
end P;
