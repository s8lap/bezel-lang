//! case: s14_2_6619
//! doc: spec
//! section: 14.2
//! title: two renamed method type parameters keep their own inherited bounds
//! rule: the inherited set is read under the implemented interface's or overridden base's substitution, its method type parameters renamed positionally
//! cites: 14.2
//! expect: run
//! output: 8
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

interface IPair<A, B>
  method Z<S, R>(a: S, b: R): int where S: IBox<A> where R: IBox<B>;
end IPair;
class D: IPair<Num, Other>
  public constructor()
  end constructor;
  public method Z<V1, V2>(a: V1, b: V2): int where V1: IBox<Num> where V2: IBox<Other> => a.Tag() + b.Tag();
end D;
class P
  public static method Main(): int

    var d := new D();
    Console.WriteLine(d.Z(new NB(), new OB()));
    return 0;
  end Main;
end P;
