//! case: s9_9_0003
//! doc: spec
//! section: 9.9
//! title: a constructor-only record does not satisfy where T colon new()
//! rule: Port at a where T: new() substitution is also rejected.
//! cites: 14.2, 17.11
//! expect: reject
//! diagnostic: constructor-only
module Conf;

public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;

public class Factory
  public static method Make<T>(): T where T: new() => new T();
end Factory;

public class Program
  public static method Build(): Port => Factory.Make<Port>();
end Program;
