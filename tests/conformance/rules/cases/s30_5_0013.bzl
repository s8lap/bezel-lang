//! case: s30_5_0013
//! doc: spec
//! section: 30.5
//! title: Task.FromResult with a class-typed T is rejected
//! rule: The factory `Task.FromResult<T>(value: T)` ... is restricted to
//!   non-class T ...; calling it with a class-typed T is a compile error.
//! cites: 30.5, 30.10
//! expect: reject
//! diagnostic: non-class
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public async method LoadAsync(): Task
    using var t := Task.FromResult<Widget>(new Widget());
    await Task.Delay(0);
  end LoadAsync;
end Program;
