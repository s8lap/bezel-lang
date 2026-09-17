//! case: l2_21_0002
//! doc: stdlib
//! section: 2.21
//! title: Dismiss() suppresses the guard's action
//! rule: Dismiss() disarms the guard; after Dismiss() it is a no-op — the
//!   action does not run at disposal.
//! cites: 2.21
//! expect: run
//! stdout: kept
module Conf;

public class Program
  public static method Main()
    do
      using var g := new ScopeGuard(lambda() => Console.WriteLine("undone"));
      g.Dismiss();
    end;
    Console.WriteLine("kept");
  end Main;
end Program;
