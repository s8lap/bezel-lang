//! case: s7_4_0153
//! doc: spec
//! section: 7.4
//! title: a test on a parameter does not reach a lambda formed under it, so Math.Min in the lambda is rejected
//! rule: so a test on a `var` local or a parameter does not reach a lambda formed under it, whether or not the body writes the variable
//! cites: 2.2, 27.5
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: Twin: s7_4_0154 copies the narrowed value into a const inside the guard, the remedy the same paragraph names.
module Conf;

public class Program
  public static method Run(d: int?): int
    if d <> null then
      var g := lambda() => Math.Min(d, 5);
      return g();
    end if;
    return 0;
  end Run;

  public static method Main()
    Console.WriteLine(Run(3));
  end Main;
end Program;
