//! case: s14_2_6623
//! doc: spec
//! section: 14.2
//! title: a bound inherited at `U := Num` may be restated as `where T: Num`
//! rule: the inherited set is read under the implemented interface's or overridden base's substitution, its method type parameters renamed positionally
//! cites: 14.2
//! expect: run
//! output: 7
//! issue: 8684
module Conf;
class Num
  public constructor()
  end constructor;
  public method Tag(): int => 7;
end Num;
interface IR<U>
  method M<S>(x: S): int where S: U;
end IR;
class D: IR<Num>
  public constructor()
  end constructor;
  public method M<T>(x: T): int where T: Num => x.Tag();
end D;
class P
  public static method Main(): int

    var d := new D();
    Console.WriteLine(d.M(new Num()));
    return 0;
  end Main;
end P;
