//! case: s28_8_1_0011
//! doc: spec
//! section: 28.8.1
//! title: a destructor body takes its own non-nullable field bare and discharges it like any other owned value
//! rule: Inside a destructor body, a field of the object being torn down is
//!   read for final disposal, and may be taken bare whatever its
//!   nullability ... The taken value must still be discharged through one
//!   of the standard mechanisms — assigned to a local with `using var`,
//!   passed as `owned` to a helper, or explicitly disposed.
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
