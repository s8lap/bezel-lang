//! case: s30_8_4502
//! doc: spec
//! section: 30.8
//! title: a Task.Delay awaited while another task cancels its source completes with
//!   OperationCanceledException
//! rule: Cancellation typically happens at `await` points: the inner call observes
//!   the request and completes with `OperationCanceledException`.
//! cites: 30.8
//! expect: run
//! output: x=42
//! issue: 7956
//! notes: Also stated by stdlib §6.7 *`Delay` observes its token at the cancellation* (v0.361, #8486).
module Conf;
import System.Threading;

public class Program
  static async method Canceller(src: CancellationTokenSource): Task
    await Task.Delay(20);
    src.Cancel();
  end Canceller;
  public static async method Main(): Task<int>
    using var cts := new CancellationTokenSource();
    var c := Canceller(cts);
    var x := 0;
    try
      await Task.Delay(5000, cts.Token);
      x := 1;
    catch var e: OperationCanceledException =>
      x := 42;
    end;
    await c;
    Console.WriteLine($"x={x}");
    return x;
  end Main;
end Program;
