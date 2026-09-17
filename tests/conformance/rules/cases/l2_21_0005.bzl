//! case: l2_21_0005
//! doc: stdlib
//! section: 2.21
//! title: Dismiss() is idempotent
//! rule: Dismiss() disarms; idempotent — a second call raises nothing.
//! cites: 2.21
//! expect: run
//! output: no error
module Conf;

public class Program
  public static method Main()
    using var g := new ScopeGuard(lambda() => Console.WriteLine("undone"));
    g.Dismiss();
    g.Dismiss();
    Console.WriteLine("no error");
  end Main;
end Program;
