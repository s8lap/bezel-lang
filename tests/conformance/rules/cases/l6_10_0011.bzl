//! case: l6_10_0011
//! doc: stdlib
//! section: 6.10
//! title: passing a new TaskScope to an owned IAsyncDisposable parameter is rejected
//! rule: Every other owning position is a compile error naming this rule ...
//!   and an owning position of a wider static type reached by a language spec
//!   §5.1 reference conversion (an `owned Object` return, an
//!   `owned IAsyncDisposable` parameter, an `await using var` declared at
//!   `Object` or at an interface).
//! cites: 6.10, 5.1, 28.1
//! expect: reject
//! diagnostic: await using
//! issue: 5657
module Conf;

import System.Threading;

public class Program
  public static method Take(owned d: IAsyncDisposable)
  end Take;

  public method Run()
    Take(new TaskScope());
  end Run;
end Program;
