//! case: s9_5_0006
//! doc: spec
//! section: 9.5
//! title: with does not re-invoke the constructor and so does not re-validate cross-field invariants
//! rule: with copies field values directly; it does not invoke the record's
//!   constructor. A constructor that validates cross-field invariants is not
//!   re-run during with.
//! expect: run
//! stdout: 1,0
module Conf;

public record Bounds
  public readonly property Lo: int;
  public readonly property Hi: int;

  public constructor(lo: int, hi: int)
    if lo > hi then
      Lo := hi;
      Hi := lo;
    else
      Lo := lo;
      Hi := hi;
    end if;
  end constructor;
end Bounds;

public class Program
  public static method Main()
    var b := new Bounds(1, 5);
    var bad := b with(Hi := 0);
    Console.WriteLine($"{bad.Lo},{bad.Hi}");
  end Main;
end Program;
