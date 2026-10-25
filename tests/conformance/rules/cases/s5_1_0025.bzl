//! case: s5_1_0025
//! doc: spec
//! section: 5.1
//! title: an int argument to Convert's string parameter is rejected
//! rule: stdlib §2.3 declares `ToInt32(value: string): int` - one parameter, of
//!   type `string`. §5.1's implicit-conversion table admits no conversion from
//!   `int` to `string` (a `string` is not a numeric type and no reference
//!   conversion reaches it), so the sole candidate of §22.6 is inapplicable and
//!   the call is a compile error.
//! cites: 5.1, 2.3, 22.6
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    _ := Convert.ToInt32(1);
  end Main;
end Program;
