//! case: s30_11_0011
//! doc: spec
//! section: 30.11
//! title: SetCancelled with one argument is rejected on the owning TaskCompletionSource
//! rule: OwningTaskCompletionSource<T> declares SetCancelled() with no parameter (spec 30.11, stdlib 6.6), so a call SetCancelled(1) has no matching overload.
//! cites: 30.11
//! expect: reject
//! diagnostic: SetCancelled
//! issue: 6077
module Conf;

import System.Threading;

public class C
  public static method Main(): int
    using var t := new OwningTaskCompletionSource<Res>();
    t.SetCancelled(1);
    return 0;
  end Main;
end C;

public class Res
end Res;
