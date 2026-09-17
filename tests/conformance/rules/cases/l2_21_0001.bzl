//! case: l2_21_0001
//! doc: stdlib
//! section: 2.21
//! title: a ScopeGuard runs its action at disposal unless dismissed
//! rule: ScopeGuard's constructor runs the action at disposal unless
//!   dismissed.
//! cites: 2.21, 28.14
//! expect: run
//! stdout: undone
module Conf;

public class Program
  public static method Main()
    do
      using var g := new ScopeGuard(lambda() => Console.WriteLine("undone"));
    end;
    Console.WriteLine("after");
  end Main;
end Program;
