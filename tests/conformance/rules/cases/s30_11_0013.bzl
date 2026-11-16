//! case: s30_11_0013
//! doc: spec
//! section: 30.11
//! title: returning a nested owned-field source's Task out of the owning local's scope is a borrow escape
//! rule: returning `tcs.Task` *itself* out of the scope is the return of a borrow sourced at a local with no source that outlives the call
//! cites: 30.11, 28.3
//! expect: reject
//! diagnostic: would outlive its source
//! issue: 8616
module Conf;

import System.Threading;

public class H
  public owned tcs: TaskCompletionSource<int> := new TaskCompletionSource<int>();
end H;
public class C
  public static method Get(): Task<int>
    using var h := new H();
    h.tcs.SetResult(1);
    return h.tcs.Task;
  end Get;
  public static method Main(): int return 0; end Main;
end C;
