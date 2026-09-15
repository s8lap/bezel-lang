//! case: s30_1_0016
//! doc: spec
//! section: 30.1
//! title: a borrow-bearing record element type is rejected on an async iterator
//! rule: An `async` method, `async` lambda, or async iterator may not
//!   declare ... a result/element type that is such a record or choice.
//! cites: 30.1, 9.7
//! expect: reject
//! diagnostic: borrow-bearing
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Wrap(W: Widget);

public class Stream
  public async method AllAsync(w: Widget): owned IAsyncEnumerable<Wrap>
    await Task.Delay(0);
    yield return new Wrap(w);
  end AllAsync;
end Stream;
