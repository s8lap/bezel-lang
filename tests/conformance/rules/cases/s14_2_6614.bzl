//! case: s14_2_6614
//! doc: spec
//! section: 14.2
//! title: a restatement in another constraint order is the same set
//! rule: the clauses may instead be restated in an equivalent form — the same constraint set per type parameter, compared order-insensitively and after type-alias resolution
//! cites: 14.2
//! expect: run
//! output: 7
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
  method Z<S>(a: S): int where S: IBox<T>, IMark;
end IQ;
class D: IQ<Num>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IMark, IBox<Num> => a.Tag() + a.Mark();
end D;
class P
  public static method Main(): int

    var d := new D();
    Console.WriteLine(d.Z(new NBM()));
    return 0;
  end Main;
end P;
