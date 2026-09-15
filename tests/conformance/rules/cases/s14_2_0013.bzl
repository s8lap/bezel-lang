//! case: s14_2_0013
//! doc: spec
//! section: 14.2
//! title: a choice never satisfies where T: new()
//! rule: Choices never satisfy the value-type arm of `where T: new()` — a
//!   consequence of the property: no choice is defaultable (§14.2, §17.11).
//! cites: 14.2, 17.11
//! expect: reject
//! diagnostic: new()
module Conf;

public choice Outcome
  Ok(value: int),
  Failed;
end Outcome;

public static class Factory
  public static method Make<T>(): owned T where T: new() => new T();
end Factory;

public class Program
  public static method Main()
    var o := Factory.Make<Outcome>();
  end Main;
end Program;
