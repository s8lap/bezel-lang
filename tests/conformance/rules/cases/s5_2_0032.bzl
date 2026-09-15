//! case: s5_2_0032
//! doc: spec
//! section: 5.2
//! title: the constant-operand rejection holds in unreachable code
//! rule: A faulting constant operand is rejected **where it is written** — the
//!   posture §5.1's constant narrowing, §5.4's constant enum cast, §6.5's
//!   constant shift count and §17.14's constant evaluation all take, which
//!   reject a faulting constant whatever its reachability.
//! cites: 5.2, 17.14
//! expect: reject
//! diagnostic: constant
//! issue: 4405
module Conf;

public class Program
  public method Run()
    if false then
      var u := uint(-1);
      Console.WriteLine(u.ToString());
    end;
  end Run;
end Program;
