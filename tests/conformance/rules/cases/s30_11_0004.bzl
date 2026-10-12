//! case: s30_11_0004
//! doc: spec
//! section: 30.11
//! title: TrySetResult returns false once the task is already in a terminal state
//! rule: The `Set*` methods complete the task; `TrySet*` returns `false`
//!   instead of throwing when the task is already in a terminal state.
//! cites: 30.11
//! expect: run
//! output: True
//!   False
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var tcs := new TaskCompletionSource<int>();
    Console.WriteLine(tcs.TrySetResult(1));
    Console.WriteLine(tcs.TrySetResult(2));
  end Main;
end Program;
