//! case: s28_8_1_0011
//! doc: spec
//! section: 28.8.1
//! title: a move inside a destructor body needs no restoration and is discharged like any other owned value
//! rule: Inside a destructor body, the field-restoration rule does not
//!   require reassignment (K9.3 — teardown, no future read); the taken
//!   value must still be discharged, for example with `using var`.
//! cites: 28.8.1
//! expect: run
//! output: holder teardown
//!   dispose inner
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

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget("inner");
  end constructor;

  destructor
    using var taken := move _resource;
    Console.WriteLine("holder teardown");
  end destructor;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
  end Main;
end Program;
