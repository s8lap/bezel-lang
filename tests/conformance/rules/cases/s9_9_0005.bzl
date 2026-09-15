//! case: s9_9_0005
//! doc: spec
//! section: 9.9
//! title: a constructor-only record is otherwise ordinary and remains auto-hashable
//! rule: In every other respect such a record is ordinary; a constructor is a
//!   member section 9.2 admits regardless of what its body does, so Port
//!   remains auto-hashable, hashable, and serialization-eligible.
//! cites: 9.2
//! expect: run
//! stdout: True
module Conf;

public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;

public class Program
  public static method Main()
    var a := new Port(8080);
    var b := new Port(8080);
    Console.WriteLine(a = b);
  end Main;
end Program;
