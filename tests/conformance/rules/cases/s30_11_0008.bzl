//! case: s30_11_0008
//! doc: spec
//! section: 30.11
//! title: SetCancelled with one argument is rejected on the generic TaskCompletionSource
//! rule: The generic TaskCompletionSource<T> declares SetCancelled() with no parameter (stdlib 6.6 and spec 30.11 show public method SetCancelled(); on the generic form), so a call SetCancelled(1) has no matching overload.
//! cites: 30.11
//! expect: reject
//! diagnostic: SetCancelled
//! issue: 6077
module Conf;

import System.Threading;

public class C
  public static method Main(): int
    using var t := new TaskCompletionSource<int>();
    t.SetCancelled(1);
    return 0;
  end Main;
end C;
