//! case: s22_10_0024
//! doc: spec
//! section: 22.10
//! title: a collection literal in the base clause of a primary constructor reaches an owned base parameter
//! rule: "Parameters used only in field initializers or in the inheritance clause's base call are not captured: their values flow directly into the field they initialize or the base argument they supply" (§22.10, v0.340); an `owned` parameter of the base primary constructor receives the literal by transfer (§22.3, §28.8). The derived classes `DOI(): OBI([1, 2, 3])`, `DOS()` and `DOR()` pass ints, strings and records; each base object owns its array, so `N()` reads 3, 4 and 2.
//! cites: 22.3, 28.8
//! expect: run
//! stdout: 3 4 2
module Conf;

public record Product(Name: string, Qty: int);

public class OBI(owned xs: Array<int>)
  public method N(): int => xs.Count;
end OBI;

public class OBS(owned xs: Array<string>)
  public method N(): int => xs.Count + xs[1].Length;
end OBS;

public class OBR(owned xs: Array<Product>)
  public method N(): int => xs.Count + xs[0].Qty;
end OBR;

public class DOI(): OBI([1, 2, 3])
end DOI;

public class DOS(): OBS(["a", "bb"])
end DOS;

public class DOR(): OBR([new Product("x", 1)])
end DOR;

public class Program
  public static method Main()
    var a := new DOI();
    var b := new DOS();
    var c := new DOR();
    Console.WriteLine($"{a.N()} {b.N()} {c.N()}");
  end Main;
end Program;
