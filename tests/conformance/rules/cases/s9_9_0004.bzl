//! case: s9_9_0004
//! doc: spec
//! section: 9.9
//! title: an ordinary body record without a contract-clause constructor satisfies where T colon new()
//! rule: A value-type substitution satisfies the new() constraint exactly when it
//!   is defaultable; a record with no contract-clause constructor is not
//!   constructor-only and remains ordinary.
//! cites: 14.2, 17.11, 28.3
//! expect: accept
module Conf;

public record Coordinate
  public readonly property Value: int;

  public constructor(value: int)
    Value := value;
  end constructor;
end Coordinate;

public class Factory
  public static method Make<T>(): owned T where T: new() => new T();
end Factory;

public class Program
  public static method Build(): Coordinate => Factory.Make<Coordinate>();
end Program;
