//! case: s14_2_4921
//! doc: spec
//! section: 14.2
//! title: a method implementing two interface methods whose sets differ only in clause order has one set
//! rule: the clauses may instead be restated in an equivalent form — the same constraint set per type parameter, compared order-insensitively and after type-alias resolution
//! cites: 14.1, 27.8
//! expect: run
//! output: 3
//! issue: 8618
module Conf;
interface IA
  static method MakeA(): int;
end IA;
interface IB
  static method MakeB(): int;
end IB;
class AB: IA, IB
  public constructor()
  end constructor;
  public static method MakeA(): int => 1;
  public static method MakeB(): int => 2;
end AB;
class OnlyA: IA
  public constructor()
  end constructor;
  public static method MakeA(): int => 1;
end OnlyA;
interface I1
  method Z<T>(a: T): int where T: IA, IB;
end I1;
interface I2
  method Z<T>(a: T): int where T: IB, IA;
end I2;
class D: I1, I2
  public constructor()
  end constructor;
  public method Z<T>(a: T): int => T.MakeA() + T.MakeB();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(new AB()));
    return 0;
  end Main;
end P;
