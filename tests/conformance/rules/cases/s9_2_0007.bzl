//! case: s9_2_0007
//! doc: spec
//! section: 9.2
//! title: an auto-hashable value-like choice is an admissible component type
//! rule: Admissible component types include a value-like choice that is itself
//!   auto-hashable, per section 35.7, recursively applying this same list to
//!   every case's payload fields.
//! cites: 35.7
//! expect: run
//! stdout: True
module Conf;

public choice Status
  Active,
  Inactive(reason: string);
end Status;

public record Job
  public readonly property State: Status;

  public constructor(state: Status)
    State := state;
  end constructor;
end Job;

public class Program
  public static method Main()
    var a := new Job(Status.Active);
    var b := new Job(Status.Active);
    Console.WriteLine(a = b);
  end Main;
end Program;
