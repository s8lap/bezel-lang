//! case: s22_6_0116
//! doc: spec
//! section: 22.6
//! title: a Convert call whose operand matches a parameter type selects that overload
//! rule: If no candidate is applicable at all — no candidate is reachable: a
//!   conversion §5.1 does not admit as implicit, a parameter-count difference
//!   at a function-typed slot, a method-reference formation that fails, or an
//!   inference that finds no binding — the call is a compile error with no
//!   candidates cited, because no overload was reachable.
//! cites: 22.6, 2.3
//! expect: run
//! stdout: 1.5

module Conf;

public class Program
  public static method Main()
    var v := Convert.ToDouble("1.5");
    Console.WriteLine(v.ToString());
  end Main;
end Program;
