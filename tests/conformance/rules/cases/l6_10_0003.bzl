//! case: l6_10_0003
//! doc: stdlib
//! section: 6.10
//! title: await Join() is mandatory on the normal path
//! rule: await Join() is mandatory on the normal path (compile error where
//!   provable, InvalidOperationException from DisposeAsync otherwise): the
//!   join-and-re-raise happens at a source-visible await, never at an
//!   unmarked end.
//! cites: 6.10, 28.14
//! expect: reject
//! diagnostic: Join
module Conf;

import System.Threading;

public class Program
  public static async method Run(): Task
    await using var scope := new TaskScope();
    scope.Spawn(lambda() => Task.Delay(0));
  end Run;
end Program;
