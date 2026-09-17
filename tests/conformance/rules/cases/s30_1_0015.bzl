//! case: s30_1_0015
//! doc: spec
//! section: 30.1
//! title: a borrow-bearing record result type is rejected on an async method
//! rule: An `async` method ... may not declare ... a result/element type
//!   that is such a record or choice.
//! cites: 30.1, 9.7
//! expect: reject
//! diagnostic: borrow-bearing
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Wrap(W: Widget);

public class Program
  public async method GetAsync(w: Widget): Task<Wrap>
    await Task.Delay(0);
    return new Wrap(w);
  end GetAsync;
end Program;
