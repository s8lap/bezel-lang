//! case: s14_2_1803
//! doc: spec
//! section: 14.2
//! title: one method per instantiation of a generic interface inherits the constraint of its own instantiation
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
//!   11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IG<U>
  method S<T>(a: Array<T>, u: U): int where T: INumber<T>;
end IG;
class D: IG<int>, IG<string>
  public method S<T>(a: Array<T>, u: int): int => a.Sum();
  public method S<T>(a: Array<T>, u: string): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a, 1));
    Console.WriteLine(d.S(a, "x"));
  end Main;
end Program;
