//! case: s30_11_0020
//! doc: spec
//! section: 30.11
//! title: await tcs.Task! inside the async method that owns the using var source is admitted
//! rule: so `return await tcs.Task;` inside the scope (every example below) is well-formed
//! cites: 30.11, 17.8, 28.3
//! expect: run
//! stdout: 7
//! issue: 8671
module Conf;
import System.Threading;

public class Program
  public static async method Get(): Task<int>
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetResult(7);
    return await tcs.Task!;
  end Get;

  public static method Main()
    using var t := Get();
    Console.WriteLine(t.Result);
  end Main;
end Program;
