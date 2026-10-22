//! case: s22_10_0026
//! doc: spec
//! section: 22.10
//! title: a generic derived class passes a literal in its base clause to an owned base parameter
//! rule: "Parameters used only in field initializers or in the inheritance clause's base call are not captured: their values flow directly into the field they initialize or the base argument they supply" (§22.10, v0.340); an `owned` parameter of the base primary constructor receives the literal by transfer (§22.3, §28.8). `D<T>(d: T): GB<T>([d, d])` gives the base an owned two-element array at `T = int` and `T = string`: `2 4 2 zz`.
//! cites: 14.1, 28.8
//! expect: run
//! stdout: 2 4 2 zz
module Conf;

public class GB<T>(owned bs: Array<T>)
  public method BL(): int => bs.Count;
end GB;

public class D<T>(d: T): GB<T>([d, d])
  public method DL(): T => d;
end D;

public class Program
  public static method Main()
    var a := new D<int>(4);
    var b := new D<string>("zz");
    Console.WriteLine($"{a.BL()} {a.DL()} {b.BL()} {b.DL()}");
  end Main;
end Program;
