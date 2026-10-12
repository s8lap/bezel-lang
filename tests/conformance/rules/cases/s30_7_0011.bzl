//! case: s30_7_0011
//! doc: spec
//! section: 30.7
//! title: a cancelled with-token faults the in-flight MoveNextAsync before any value is yielded
//! rule: **Token routing in generated async iterators.** For a
//!   compiler-generated async iterator, the enumerator returned by the
//!   generated `GetAsyncEnumerator(token)` captures the token, and every
//!   `MoveNextAsync()` performs an implicit `token.ThrowIfCancellationRequested()`
//!   — on entry ... so a requested cancellation faults the in-flight
//!   `MoveNextAsync` task with `OperationCanceledException`.
//! cites: 30.7, 30.8
//! expect: run
//! stdout: cancelled
module Conf;
import System.Threading;

public class Src
  public static async method LinesAsync(): owned IAsyncEnumerable<int>
    yield return 1;
    yield return 2;
  end LinesAsync;
end Src;

public class Program
  public static async method Main(): Task
    using var cts := new CancellationTokenSource();
    cts.Cancel();
    try
      await for var n in Src.LinesAsync() with cts.Token do
        Console.WriteLine(n);
      end;
      Console.WriteLine("completed");
    catch
      var ex: OperationCanceledException => Console.WriteLine("cancelled")
    end try;
  end Main;
end Program;
