//! case: l6_10_0013
//! doc: stdlib
//! section: 6.10
//! title: a var borrow alias of the await using scope is admitted and usable
//! rule: Borrows are otherwise unrestricted — a plain parameter at
//!   `TaskScope`, at `Object` or at an interface, a pattern binding, a child
//!   body's self-capture.
//! cites: 6.10, 28.10, 21.1
//! expect: accept
//! issue: 5657
module Conf;

import System.Threading;

public class Program
  public static async method Run(): Task
    await using var scope := new TaskScope();
    var alias := scope;
    alias.Spawn(lambda() => Task.Delay(0));
    await scope.Join();
  end Run;
end Program;
