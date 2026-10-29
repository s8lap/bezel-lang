//! case: l9_1_0023
//! doc: stdlib
//! section: 9.1
//! title: a string radix argument matches no radix parameter and is a compile error
//! rule: stdlib §9.1 (v0.350) declares `public static pure method Parse(s: string, radix: int): int;`,
//!   so the second argument must be an int. Language spec §5.1 (v0.350): "A value of one type
//!   implicitly converts to another when no programmer-visible information is lost", and no
//!   implicit conversion has string as its source. Language spec §22.6 (v0.350): "with no
//!   applicable candidate at all, the no-candidate error".
//! cites: 9.1, 5.1, 22.6
//! expect: reject
//! diagnostic: applicable candidate
//! notes: harvested from the R36 reviewer probe p15_string_radix. Its accepting twin is
//!   l9_1_0024, the same call with the int radix 16. The uint and bool radix probes (p02, p14)
//!   are the same shape at the same position and are not separately harvested.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("ff", "16"));
  end Main;
end Program;
