//! case: s30_11_0007
//! doc: spec
//! section: 30.11
//! title: TrySetResult with one argument is rejected on the non-generic TaskCompletionSource
//! rule: The non-generic TaskCompletionSource has the same shape as the generic one minus the result parameter on SetResult and TrySetResult, so SetResult and TrySetResult take no argument on it (the declared shape is public method SetResult(); and TrySetResult(): bool;). The stdlib declares the non-generic SetResult() and TrySetResult(): bool the same way (stdlib 6.6). A call TrySetResult(1) therefore has no matching overload.
//! cites: 30.11
//! expect: reject
//! diagnostic: TrySetResult
//! issue: 6064
module Conf;

import System.Threading;

public class C
  public static method Main(): int
    using var tcs := new TaskCompletionSource();
    var ok: bool := tcs.TrySetResult(1);
    return 0;
  end Main;
end C;
