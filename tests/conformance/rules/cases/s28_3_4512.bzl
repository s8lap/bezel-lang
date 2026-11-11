//! case: s28_3_4512
//! doc: spec
//! section: 28.3
//! title: a bound lazy query whose enumeration right is spent by Count in a hole draws
//!   no must-use warning
//! rule: 4. for a **bound lazy query**, the binding's **enumeration right is spent**
//!   at one of
//! cites: 29.11
//! expect: quiet
//! issue: 8392
module Conf;

public class Program
  public static method Exercise()
    var names: Array<string> := ["a", "bb", "ccc"];
    var q := names.Where(lambda(n) => n.Length > 1);
    Console.WriteLine($"n={q.Count()}");
  end Exercise;
end Program;
