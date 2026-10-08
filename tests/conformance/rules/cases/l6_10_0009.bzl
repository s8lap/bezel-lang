//! case: l6_10_0009
//! doc: stdlib
//! section: 6.10
//! title: returning a new TaskScope through an owned Object return slot is rejected
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

public class Factory
  public static method Make(): owned Object
    return new TaskScope();
  end Make;
end Factory;
