//! case: s14_2_8103
//! doc: spec
//! section: 14.2
//! title: a nullable stdlib record does not satisfy where T: value at a call site
//! rule: A nullable value type (e.g. `int?`) does **not** satisfy `value`.
//! cites: 14.2
//! expect: reject
//! diagnostic: value
//! notes: Boundary of #8733: admitting the stdlib record builtins at `value` must not admit
//!  their nullable form. Twin: s14_2_8101.
module Conf;
public class P
  public static method F<T>(x: T): int where T: value
    return 1;
  end F;
  public static method Main(): int
    var t: TimeSpan? := TimeSpan.FromTicks(5L);
    Console.WriteLine(P.F(t));
    return 0;
  end Main;
end P;
