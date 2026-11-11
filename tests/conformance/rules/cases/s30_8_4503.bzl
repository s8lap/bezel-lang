//! case: s30_8_4503
//! doc: spec
//! section: 30.8
//! title: a Task.Delay started on an already-cancelled token completes with
//!   OperationCanceledException
//! rule: Cancellation typically happens at `await` points: the inner call observes
//!   the request and completes with `OperationCanceledException`.
//! cites: 30.8
//! expect: run
//! output: n=3
//! issue: 7956
module Conf;
import System.Threading;

public class Program
  public static async method Main(): Task<int>
    var n := 0;
    for var i in 0..<3 do
      using var cts := new CancellationTokenSource();
      cts.Cancel();
      try
        await Task.Delay(1000, cts.Token);
      catch var e: OperationCanceledException =>
        n := n + 1;
      end;
    end for;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end Program;
