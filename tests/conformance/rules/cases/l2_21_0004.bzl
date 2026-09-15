//! case: l2_21_0004
//! doc: stdlib
//! section: 2.21
//! title: IsDismissed defaults to False before Dismiss() is called
//! rule: IsDismissed exists for assertions and tests; a fresh guard is not
//!   dismissed.
//! cites: 2.21
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    using var g := new ScopeGuard(lambda() => Console.WriteLine("undone"));
    Console.WriteLine(g.IsDismissed);
    g.Dismiss();
  end Main;
end Program;
