//! case: s30_11_0014
//! doc: spec
//! section: 30.11
//! title: returning a generic source's Task out of its scope is a borrow escape at an unconstrained T
//! rule: returning `tcs.Task` *itself* out of the scope is the return of a borrow sourced at a local with no source that outlives the call
//! cites: 30.11, 28.3
//! expect: reject
//! diagnostic: would outlive its source
//! issue: 8616
module Conf;

import System.Threading;

public class C
  public static method Get<T>(): Task<T>
    using var tcs := new TaskCompletionSource<T>();
    return tcs.Task;
  end Get;
  public static method Main(): int return 0; end Main;
end C;
