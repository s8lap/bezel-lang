//! case: s14_2_1801
//! doc: spec
//! section: 14.2
//! title: an overload beside a constrained interface method inherits only its own declaration's constraints
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
//!   4040
//! issue: 8184
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  method S<T>(a: Array<T>, k: int): int where T: INumber<T>;
  method S<T>(a: Array<T>, k: string): int;
end IA;
class D: IA
  public method S<T>(a: Array<T>, k: string): int => a.Sum();
  public method S<T>(a: Array<T>, k: int): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a, 1));
    Console.WriteLine(d.S(a, "k"));
  end Main;
end Program;
