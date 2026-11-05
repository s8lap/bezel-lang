//! case: s14_2_1802
//! doc: spec
//! section: 14.2
//! title: a method matching no interface method inherits no constraint from a same-named one
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 4040
//! issue: 8184
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  method S<T>(s: string): int where T: INumber<T>;
end IA;
class D: IA
  public method S<T>(a: Array<T>): int => a.Sum();
  public method S<T>(s: string): int => 1;
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
