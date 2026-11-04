//! case: s9_3_0008
//! doc: spec
//! section: 9.3
//! title: a positional record body field of non-defaultable type with an initializer is admitted
//! rule: so a non-defaultable body field or auto-property is satisfied only by a field initializer ([§22.8](#sec-field-initializers)); a body constructor, of any parameter list, does not assign it for the primary.
//! cites: 9.3, 7.9, 22.8
//! expect: run
//! issue: 8097
//! output: 1
//!   q
module Conf;

public record Holder(Seed: int)
  private _c: string := "x";

  public constructor(seed: int, c: string)
    Seed := seed;
    _c := c;
  end constructor;

  public method C(): string => _c;
end Holder;

public class Program
  public static method Main()
    var h := new Holder(1, "q");
    Console.WriteLine(h.Seed);
    Console.WriteLine(h.C());
  end Main;
end Program;
