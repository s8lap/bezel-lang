//! case: s35_4_0004
//! doc: spec
//! section: 35.4
//! title: disposing an owning choice disposes the active case's owned payloads in declaration order
//! rule: Disposal is defined by the active case. Disposing an owning choice value
//!   disposes the active case's owned payloads in payload declaration order;
//!   payload-free and value-payload cases dispose nothing.
//! expect: run
//! output: dispose first
//!   dispose second
module Conf;

public class Resource
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Resource;

public choice Bundle
  Ready(owned first: Resource, owned second: Resource),
  Empty;
end Bundle;

public class Program
  public static method Main()
    using var b := Bundle.Ready(new Resource("first"), new Resource("second"));
  end Main;
end Program;
