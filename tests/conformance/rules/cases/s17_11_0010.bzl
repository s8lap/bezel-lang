//! case: s17_11_0010
//! doc: spec
//! section: 17.11
//! title: default of a constructor-only record is rejected
//! rule: "A **constructor-only** record (§9.9) — a record declaring a
//!   constructor carrying a contract clause. Every value of such a record is
//!   produced by a call to one of its declared constructors, and
//!   `default(T)` calls none."
//! cites: 17.11, 9.9
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

public class Program
  public static method Main()
    var p := default(Port);
  end Main;
end Program;
