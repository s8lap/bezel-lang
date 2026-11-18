//! case: s14_2_6613
//! doc: spec
//! section: 14.2
//! title: a restated `where S: IBox<Num>` is the inherited `where S: IBox<T>` under `T := Num`
//! rule: the inherited set is read under the implemented interface's or overridden base's substitution, its method type parameters renamed positionally
//! cites: 14.2
//! expect: run
//! output: 7
//! issue: 8684
module Conf;
class Num
  public constructor()
  end constructor;
end Num;
interface IBox<T>
  method Z<S>(a: S): int where S: IBox<T>;
  method Tag(): int;
end IBox;
class D: IBox<Num>
  public constructor()
  end constructor;
  public method Tag(): int => 7;
  public method Z<S>(a: S): int where S: IBox<Num> => a.Tag();
end D;
class P
  public static method Main(): int

    var d := new D();
    Console.WriteLine(d.Z(new D()));
    return 0;
  end Main;
end P;
