//! case: s14_2_1818
//! doc: spec
//! section: 14.2
//! title: across modules an implemented interface method and an overridden virtual inherit constraints
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2, 13.1
//! expect: run
//! output: 11
//!   11
module App;
import Lib;
extension Object
  public method Sum(): int => 4040;
end;
class D: B, IA
  public method S<T>(a: Array<T>): int => a.Sum();
  public method R<T>(s: string): int => 1;
  public override method V<T>(a: Array<T>): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a));
    Console.WriteLine(d.V(a));
  end Main;
end Program;
