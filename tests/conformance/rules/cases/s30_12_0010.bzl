//! case: s30_12_0010
//! doc: spec
//! section: 30.12
//! title: a LockGuard is not sendable and cannot be a channel element
//! rule: Guards are domain-bound and **not sendable**, like `Lock` and
//!   `SemaphoreToken`.
//! cites: 30.12, 30.15, 30.14
//! expect: reject
//! diagnostic: sendable
//! issue: 1721
//! notes: the interior class Counter is sendable (sealed, int field), so the
//!   only failing component is the guard type itself; `Channel<Counter>` is
//!   admitted (l6_9_0002's shape).
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public method Run()
    using var ch := new Channel<LockGuard<Counter>>();
  end Run;
end Program;
