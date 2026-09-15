//! case: s5_1_0017
//! doc: spec
//! section: 5.1
//! title: a run-time int value does not implicitly narrow to byte even when it would fit
//! rule: The [constant-narrowing] rule applies to constants only; a run-time
//!   int value still requires the explicit byte(x) narrowing of §5.2. Twin of
//!   s5_1_0014: the same target, a non-constant source.
//! cites: 5.1, 5.2
//! expect: reject
//! diagnostic: only as listed
module Conf;

public class Program
  public static method Make(n: int)
    var b: byte := n;
  end Make;
end Program;
