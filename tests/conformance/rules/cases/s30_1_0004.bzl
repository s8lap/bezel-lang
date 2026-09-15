//! case: s30_1_0004
//! doc: spec
//! section: 30.1
//! title: an async iterator may return IAsyncEnumerable<T>
//! rule: `IAsyncEnumerable<T>` — for re-iterable async iterators that
//!   contain `yield return` (§30.7). The yielded values are borrowed by the
//!   consumer.
//! cites: 30.1, 30.7
//! expect: accept
module Conf;

public class Reader
  public async method NumbersAsync(): owned IAsyncEnumerable<int>
    await Task.Delay(0);
    yield return 1;
    yield return 2;
  end NumbersAsync;
end Reader;
