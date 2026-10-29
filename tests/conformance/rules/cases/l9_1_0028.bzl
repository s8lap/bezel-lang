//! case: l9_1_0028
//! doc: stdlib
//! section: 9.1
//! title: TryParse(s, radix, out) returns false for a 0x prefix
//! rule: stdlib §9.1 (v0.350) "The radix alphabet": "`Parse(s, radix)` and `TryParse(s, radix, …)`
//!   … under the input grammar the string forms carry throughout: an optional leading `+` or `-`,
//!   then one or more digits of that radix, and nothing else; no whitespace, no `0x`/`0b` prefix".
//!   The `Try` form reports the same inputs `Parse` rejects by returning false.
//! cites: 9.1
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    var v: int := -1;
    var ok := int.TryParse("0x1F", 16, out v);
    Console.WriteLine(ok);
  end Main;
end Program;
