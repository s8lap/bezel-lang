//! case: s22_2_3605
//! doc: spec
//! section: 22.2
//! title: a valued arrow body on an async method declared Task is a compile error
//! rule: An `async` method whose declared return type is `Task` completes with no result value (§30.1),
//!   so its expression body conforms only when the expression is of type `()` (`=> await StepAsync()`): a
//!   valued body does not conform to a plain `Task` … and is a compile error at the body.
//! cites: 22.2, 30.1, 30.6
//! expect: reject
//! diagnostic: Task
//! issue: 9246
//! notes: Accepting twin s22_2_3606 (a unit-typed await).
module Conf;

public class Program
  public static method Compute(): int => 1;
  public static async method H(): Task => Program.Compute();
end Program;
