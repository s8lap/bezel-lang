//! case: s30_1_0012
//! doc: spec
//! section: 30.1
//! title: a borrow-bearing record parameter is rejected on an async method
//! rule: Borrow-bearing parameters and results are forbidden. An `async`
//!   method, `async` lambda, or async iterator may not declare a parameter
//!   whose type is a borrow-bearing record (§9.7).
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
  public async method HandleAsync(w: Wrap): Task
    await Task.Delay(0);
  end HandleAsync;
end Program;
