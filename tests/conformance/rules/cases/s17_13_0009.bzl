//! case: s17_13_0009
//! doc: spec
//! section: 17.13
//! title: the anonymous owning binding using var _ is rejected over a Task-family value
//! rule: "so is the anonymous binding `using var _ := expression;` (and
//!   `await using var _ := ...;`, below) over a `Task`-family value: nothing
//!   can await an unnamed binding, so it is the same abandonment one scope
//!   later."
//! cites: 17.13, 30.10
//! expect: reject
//! diagnostic: FireAndForget
module Conf;

public class Program
  public static method Main()
    using var _ := Task.Delay(10);
  end Main;
end Program;
