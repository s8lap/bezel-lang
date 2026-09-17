//! case: l6_10_0004
//! doc: stdlib
//! section: 6.10
//! title: an await Join() call in the method body satisfies the mandatory-join rule
//! rule: await Join() is mandatory on the normal path (compile error where
//!   provable, InvalidOperationException from DisposeAsync otherwise).
//! cites: 6.10, 28.14
//! expect: accept
module Conf;

import System.Threading;

public class Program
  public static async method Run(): Task
    await using var scope := new TaskScope();
    scope.Spawn(lambda() => Task.Delay(0));
    await scope.Join();
  end Run;
end Program;
