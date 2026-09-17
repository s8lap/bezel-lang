//! case: s9_9_0005
//! doc: spec
//! section: 9.9
//! title: a constructor-only record is otherwise ordinary and remains auto-hashable
//! rule: In every other respect such a record is ordinary; a constructor is a
//!   member section 9.2 admits regardless of what its body does, so Port
//!   remains auto-hashable, hashable, and serialization-eligible.
//! cites: 9.2
//! expect: run
//! output: True
//!   False
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
    var c := new Port(443);
    Console.WriteLine(a = b);
    Console.WriteLine(a = c);
  end Main;
end Program;
