//! case: s14_2_1806
//! doc: spec
//! section: 14.2
//! title: a method implementing a member of an inherited interface inherits its constraints
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  method S<T>(a: Array<T>): int where T: INumber<T>;
end IA;
interface IB: IA
  method Q(): int;
end IB;
class D: IB
  public method S<T>(a: Array<T>): int => a.Sum();
  public method Q(): int => 1;
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a));
  end Main;
end Program;
