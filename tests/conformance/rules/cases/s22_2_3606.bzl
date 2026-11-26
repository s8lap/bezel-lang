//! case: s22_2_3606
//! doc: spec
//! section: 22.2
//! title: a unit-typed arrow body on an async method declared Task conforms
//! rule: An `async` method whose declared return type is `Task` completes with no result value (§30.1),
//!   so its expression body conforms only when the expression is of type `()` (`=> await StepAsync()`): a
//!   valued body does not conform to a plain `Task` … and is a compile error at the body.
//! cites: 22.2, 30.1, 30.6
//! expect: accept
//! issue: 9246
module Conf;

public class Program
  public static async method H(): Task => await Task.Delay(1);
end Program;
