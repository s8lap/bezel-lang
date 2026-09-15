//! case: s35_10_0002
//! doc: spec
//! section: 35.10
//! title: choices never satisfy where T: new()
//! rule: Choices never satisfy the value-type arm of where T: new() — a consequence of
//!   the property: no choice is defaultable.
//! cites: 35.10, 14.2
//! expect: reject
//! diagnostic: new()
module Conf;

public choice Status
  Active,
  Inactive;
end Status;

public class Factory<T> where T: new()
  public static method Make(): owned T => new T();
end Factory;

public class Program
  public static method Main()
    var s := Factory<Status>.Make();   // ERROR: `Status` does not satisfy `new()` — no choice does
  end Main;
end Program;
