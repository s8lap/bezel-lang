//! case: s30_11_0010
//! doc: spec
//! section: 30.11
//! title: the zero-argument TrySetResult on the non-generic TaskCompletionSource is accepted and runs
//! rule: The non-generic TaskCompletionSource declares TrySetResult(): bool with no parameter (spec 30.11, stdlib 6.6), so a zero-argument call is the admitted form and it runs.
//! cites: 30.11
//! expect: run
//! stdout: done
//! issue: 6064
module Conf;

import System.Threading;

public class C
  public static method Main(): int
    using var tcs := new TaskCompletionSource();
    var ok: bool := tcs.TrySetResult();
    Console.WriteLine("done");
    return 0;
  end Main;
end C;
