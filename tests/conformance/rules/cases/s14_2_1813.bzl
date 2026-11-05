//! case: s14_2_1813
//! doc: spec
//! section: 14.2
//! title: a method in a generic class implementing a generic interface method inherits its constraints
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA<U>
  method S<T>(a: Array<T>, u: U): int where T: INumber<T>;
end IA;
class D<U>: IA<U>
  public method S<T>(a: Array<T>, u: U): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D<string>();
    Console.WriteLine(d.S(a, "q"));
  end Main;
end Program;
