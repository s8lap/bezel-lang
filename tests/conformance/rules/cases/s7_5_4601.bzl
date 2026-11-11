//! case: s7_5_4601
//! doc: spec
//! section: 7.5
//! title: a ?. read of a TaskCompletionSource's Task through a null receiver is null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 30.11
//! expect: run
//! stdout: False
//! issue: 7423
module Conf;

import System.Threading;

public class Program
  public static method Main()
    var tcs: TaskCompletionSource<int>? := null;
    var t := tcs?.Task;
    var done: bool? := t?.IsCompleted;
    Console.WriteLine(done ?? false);
  end Main;
end Program;
