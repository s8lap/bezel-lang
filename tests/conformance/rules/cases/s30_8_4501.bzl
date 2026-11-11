//! case: s30_8_4501
//! doc: spec
//! section: 30.8
//! title: one Cancel completes every pending Task.Delay on its token with
//!   OperationCanceledException
//! rule: Cancellation typically happens at `await` points: the inner call observes
//!   the request and completes with `OperationCanceledException`.
//! cites: 30.8
//! expect: run
//! output: n=3 calls=1
//! issue: 7956
module Conf;
import System.Threading;

public class Program
  public static async method Main(): Task<int>
    using var cts := new CancellationTokenSource();
    var tok := cts.Token;
    var t1 := Task.Delay(5000, tok);
    var t2 := Task.Delay(5000, tok);
    var t3 := Task.Delay(5000, tok);
    var calls := 0;
    using var sub := tok.Register(lambda() => do calls := calls + 1; end);
    cts.Cancel();
    var n := 0;
    try
      await t1;
    catch var e: OperationCanceledException =>
      n := n + 1;
    end;
    try
      await t2;
    catch var e: OperationCanceledException =>
      n := n + 1;
    end;
    try
      await t3;
    catch var e: OperationCanceledException =>
      n := n + 1;
    end;
    Console.WriteLine($"n={n} calls={calls}");
    return n;
  end Main;
end Program;
