//! case: s9_3_0012
//! doc: spec
//! section: 9.3
//! title: a positional record body auto-property with an initializer is admitted beside a constructor
//! rule: so a non-defaultable body field or auto-property is satisfied only by a field initializer ([§22.8](#sec-field-initializers)); a body constructor, of any parameter list, does not assign it for the primary.
//! cites: 9.3, 7.9, 22.8, 23.1
//! expect: run
//! issue: 8097
//! output: 3
//!   q
module Conf;

public record Holder(Seed: int)
  public readonly property C: string := "q";

  public constructor(seed: int, extra: int)
    Seed := seed + extra;
  end constructor;
end Holder;

public class Program
  public static method Main()
    var h := new Holder(1, 2);
    Console.WriteLine(h.Seed);
    Console.WriteLine(h.C);
  end Main;
end Program;
