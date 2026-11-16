//! case: s30_11_0012
//! doc: spec
//! section: 30.11
//! title: returning a parenthesised tcs.Task out of the source's scope is a borrow escape
//! rule: returning `tcs.Task` *itself* out of the scope is the return of a borrow sourced at a local with no source that outlives the call
//! cites: 30.11, 28.3
//! expect: reject
//! diagnostic: would outlive its source
//! issue: 8616
module Conf;

import System.Threading;

public class C
  public static method Get(): Task<int>
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetResult(1);
    return (tcs.Task);
  end Get;
  public static method Main(): int return 0; end Main;
end C;
