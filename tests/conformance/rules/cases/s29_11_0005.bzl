//! case: s29_11_0005
//! doc: spec
//! section: 29.11
//! title: passing a lazy query as an IEnumerable argument transfers its single-enumeration right to the callee
//! rule: Passing the query as an `IEnumerable<T>` argument: the
//!   single-enumeration right transfers to the callee; the frame's disposal
//!   obligation stays with the caller, the plain argument being an ordinary
//!   borrow.
//! cites: 29.11
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method CountOf(items: IEnumerable<int>): int
    return items.Count();
  end CountOf;

  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x) => x > 1);
    Console.WriteLine(CountOf(q));
  end Main;
end Program;
