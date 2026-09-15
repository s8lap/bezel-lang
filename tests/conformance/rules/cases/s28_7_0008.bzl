//! case: s28_7_0008
//! doc: spec
//! section: 28.7
//! title: reassigning an owning-inline-array static field is a plain store, never drop-on-overwrite
//! rule: A static owning-inline-array field stays a plain store — statics
//!   never take drop-on-overwrite for any type; whole-field reassignment
//!   never auto-disposes displaced slots.
//! cites: 28.7, 22.12, 22.13
//! expect: run
//! stdout: reassigned
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public static class Store
  public static _slots: [1 owned]Widget := [new Widget("first")];
end Store;

public class Program
  public static method Main()
    Store._slots := [new Widget("second")];
    Console.WriteLine("reassigned");
  end Main;
end Program;
