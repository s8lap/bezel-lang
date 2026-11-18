//! case: s27_5_7214
//! doc: spec
//! section: 27.5
//! title: a spawned child's write to a captured local is visible to the
//!   frame after Join
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa.
//! cites: 27.5, 30.12
//! expect: run
//! stdout: 220100
module Conf;

import System.Threading;

public class Program
  public static async method R(i: int): Task<int>
    var n := i + 1;
    do
      await using var scope := new TaskScope();
      scope.Spawn(async lambda() => do
        await Task.Delay(1);
        n := n + 1000;
      end);
      await scope.Join();
    end;
    return n;
  end R;

  public static async method Main(): Task<int>
    var sum := 0;
    for var i in 0..<200 do
      sum := sum + await Program.R(i);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
