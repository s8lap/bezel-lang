//! case: s29_11_0006
//! doc: spec
//! section: 29.11
//! title: reusing a lazy query at the caller after passing it as an argument is a compile error
//! rule: Passing the query as an `IEnumerable<T>` argument: the
//!   single-enumeration right transfers to the callee.
//! cites: 29.11
//! expect: reject
//! diagnostic: ToArray
module Conf;

public class Program
  public static method CountOf(items: IEnumerable<int>): int
    return items.Count();
  end CountOf;

  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x) => x > 1);
    var c := CountOf(q);
    for var v in q do
      _ := v;
    end;
  end Main;
end Program;
