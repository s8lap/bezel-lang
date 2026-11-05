//! case: s14_2_1817
//! doc: spec
//! section: 14.2
//! title: across modules constrained and unconstrained interface implementations inherit independently
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2, 13.1
//! expect: run
//! output: 11
//!   4040
//! issue: 8184
module App;
import Lib;
extension Object
  public method Sum(): int => 4040;
end;
class D: IA, IU
  public method S<T>(a: Array<T>): int => a.Sum();
  public method R<T>(s: string): int => 1;
  public method R<T>(a: Array<T>): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a));
    Console.WriteLine(d.R(a));
  end Main;
end Program;
