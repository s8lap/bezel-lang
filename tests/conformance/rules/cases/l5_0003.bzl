//! case: l5_0003
//! doc: stdlib
//! section: 5
//! title: passing a query as an IEnumerable<T> argument transfers its single-enumeration right
//! rule: Passing the query as an IEnumerable<T> argument transfers the
//!   single-enumeration right to the callee; inside a callee a plain
//!   IEnumerable<T> parameter is statically untracked, so a re-enumeration
//!   there is enforced only by the runtime InvalidOperationException guard.
//! cites: 5, 29.11
//! expect: run
//! stdout: caught
module Conf;

public method Reenumerate(source: IEnumerable<int>)
  for var v in source do
  end;
  for var v2 in source do
  end;
end Reenumerate;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x: int): bool => x > 0);
    try
      Reenumerate(q);
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
