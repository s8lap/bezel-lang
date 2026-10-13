//! case: s3_6_0007
//! doc: spec
//! section: 3.6
//! title: a function type is not hashable, so a Set over a function type is rejected
//! rule: hashable ... every type except ... (e) a function type (§27.7.1: a
//!   lambda equals only itself, so a hash container of function values would
//!   hold every lambda evaluation apart; a function value's hash agrees with
//!   that equality, and at `Object` it is an ordinary key)
//! cites: 3.6, 27.7.1, 14.2
//! expect: reject
//! diagnostic: hashable
//! issue: 5827
module Conf;

public class Demo
  public static method Twice(x: int): int => x * 2;

  public static method Run(): int
    var seen := new Set<(int) -> int>();
    seen.Add(Demo.Twice);
    return seen.Count;
  end Run;
end Demo;
