//! case: s30_11_0009
//! doc: spec
//! section: 30.11
//! title: SetResult with two arguments is rejected on the generic TaskCompletionSource
//! rule: The generic TaskCompletionSource<T> declares SetResult(result: T) with one parameter (spec 30.11), so a call SetResult(1, 2) has no matching overload.
//! cites: 30.11
//! expect: reject
//! diagnostic: SetResult
//! issue: 6077
module Conf;

import System.Threading;

public class C
  public static method Main(): int
    using var t := new TaskCompletionSource<int>();
    t.SetResult(1, 2);
    return 0;
  end Main;
end C;
