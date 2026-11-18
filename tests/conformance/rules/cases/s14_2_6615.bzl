//! case: s14_2_6615
//! doc: spec
//! section: 14.2
//! title: a restatement through a type alias of the substituted bound is the same set
//! rule: the clauses may instead be restated in an equivalent form — the same constraint set per type parameter, compared order-insensitively and after type-alias resolution
//! cites: 14.2, 27.8
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
type NumBox := IBox<Num>;

interface IQ<T>
  method Z<S>(a: S): int where S: IBox<T>;
end IQ;

class D: IQ<Num>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: NumBox => a.Tag();
end D;
class P
  public static method Main(): int

    var d := new D();
    Console.WriteLine(d.Z(new NB()));
    return 0;
  end Main;
end P;
