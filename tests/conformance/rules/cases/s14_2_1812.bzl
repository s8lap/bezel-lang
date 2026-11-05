//! case: s14_2_1812
//! doc: spec
//! section: 14.2
//! title: a renamed method type parameter corresponds by position and inherits the constraint
//! rule: The two declarations' method-level type parameters, equal in number,
//!   correspond by position
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
class D: IA
  public method S<U>(a: Array<U>): int => a.Sum();
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
