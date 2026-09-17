//! case: s3_6_0005
//! doc: spec
//! section: 3.6
//! title: a record with no contract clause on its constructor is defaultable
//! rule: a record all of whose declared fields are defaultable and which is
//!   not constructor-only is defaultable.
//! cites: 3.6, 9.9, 17.11
//! expect: run
//! stdout: 0
module Conf;

public record Port
  public readonly property Value: int;

  public constructor(value: int)
    Value := value;
  end constructor;
end Port;

public class Program
  public static method Main()
    Console.WriteLine($"{default(Port).Value}");
  end Main;
end Program;
