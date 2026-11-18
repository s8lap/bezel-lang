//! case: s30_11_0019
//! doc: spec
//! section: 30.11
//! title: returning (tcs.Task)! out of the scope of a using var source is rejected
//! rule: returning `tcs.Task` *itself* out of the scope is the return of a borrow sourced at a
//!   local with no source that outlives the call
//! cites: 30.11, 17.8, 28.3
//! expect: reject
//! diagnostic: borrow
//! issue: 8671
module Conf;
import System.Threading;

public class Program
  public static method Get(): Task<int>
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetResult(1);
    return (tcs.Task)!;
  end Get;

  public static method Main()
    using var t := Get();
    Console.WriteLine(t.Result);
  end Main;
end Program;
