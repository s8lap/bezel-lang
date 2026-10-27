//! case: s22_6_0118
//! doc: spec
//! section: 22.6
//! title: a Convert.TryToDouble call whose operand matches no parameter type is a compile error
//! rule: If no candidate is applicable at all — no candidate is reachable: a
//!   conversion §5.1 does not admit as implicit, a parameter-count difference
//!   at a function-typed slot, a method-reference formation that fails, or an
//!   inference that finds no binding — the call is a compile error with no
//!   candidates cited, because no overload was reachable.
//! cites: 22.6, 5.1, 2.3
//! expect: reject
//! diagnostic: overload
//! issue: 7323

module Conf;

public class Program
  public static method Main()
    var v: double := 0.0;
    var ok := Convert.TryToDouble(1.5, out v);
    Console.WriteLine(ok);
  end Main;
end Program;
