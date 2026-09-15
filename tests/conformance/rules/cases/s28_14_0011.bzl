//! case: s28_14_0011
//! doc: spec
//! section: 28.14
//! title: self-capture of the bounding local itself is admitted
//! rule: Self-capture — a value whose borrow provably originates from L —
//!   is admitted: scope.Spawn(lambda() => Work(scope.Token)) is the
//!   sanctioned shape.
//! cites: 28.14
//! expect: run
//! stdout: worked
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    await using var scope := new TaskScope();
    scope.Spawn(lambda() => Worker(scope.Token));
    await scope.Join();
  end Main;

  private static async method Worker(token: CancellationToken): Task
    await Task.Delay(0);
    Console.WriteLine("worked");
  end Worker;
end Program;
