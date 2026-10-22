//! case: s19_3_0072
//! doc: spec
//! section: 19.3
//! title: ulong constant and range patterns above long's maximum match a ulong scrutinee by unsigned value
//! rule: a constant pattern is the equality test of §8.4, which is total on null
//!   and does not equal a constant. A range pattern is admitted over a
//!   scrutinee whose type §16.3 admits as a range's T — the integer types and
//!   char — because its test is that range's membership (§16.6).
//! cites: 19.3, 16.6, 8.4
//! expect: run
//! output: max
//!   large
//! issue: 6916
module Conf;

public class Program
  public static method Main()
    var u: ulong := 18446744073709551615UL;
    Console.WriteLine((case u of 18446744073709551615UL => "max", _ => "other"));
    var v: ulong := 9223372036854775808UL;
    Console.WriteLine((case v of ..10UL => "small", _ => "large"));
  end Main;
end Program;
