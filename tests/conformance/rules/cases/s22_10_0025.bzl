//! case: s22_10_0025
//! doc: spec
//! section: 22.10
//! title: a collection literal in an explicit base call reaches an owned base primary-constructor parameter
//! rule: "Parameters used only in field initializers or in the inheritance clause's base call are not captured: their values flow directly into the field they initialize or the base argument they supply" (§22.10, v0.340); an `owned` parameter of the base primary constructor receives the literal by transfer (§22.3, §28.8). The explicit constructors `EOI: OBI public constructor() base([1, 2, 3]);` pass the literal by transfer to the base's owned parameter; `N()` reads 3, 4 and 2.
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

public class EOI: OBI
  public constructor() base([1, 2, 3]); end;
end EOI;

public class EOS: OBS
  public constructor() base(["a", "bb"]); end;
end EOS;

public class EOR: OBR
  public constructor() base([new Product("x", 1)]); end;
end EOR;

public class Program
  public static method Main()
    var a := new EOI();
    var b := new EOS();
    var c := new EOR();
    Console.WriteLine($"{a.N()} {b.N()} {c.N()}");
  end Main;
end Program;
