//! case: s7_1_0003
//! doc: spec
//! section: 7.1
//! title: a null literal is not passed to a non-nullable string parameter of a stdlib member
//! rule: Language spec §7.1 (v0.350): "A plain reference type is non-nullable; a `?` suffix produces
//!   its nullable form", and "The compiler enforces the distinction at every assignment, parameter
//!   pass, and return." The parameter is `s: string`, not `string?`, in stdlib §9.1's
//!   `public static pure method Parse(s: string): int;` (v0.350).
//! cites: 7.1, 9.1
//! expect: reject
//! diagnostic: non-nullable
//! notes: harvested from the R36 reviewer probe p04_long_parse_null, with int in place of long:
//!   the stdlib document spells Parse for int (§9.1) and says the other integer types have
//!   "analogous shapes", so long.Parse is not pinned as a member. Accepting twin: s7_1_0004.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse(null));
  end Main;
end Program;
