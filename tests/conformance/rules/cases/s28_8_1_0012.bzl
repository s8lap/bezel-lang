//! case: s28_8_1_0012
//! doc: spec
//! section: 28.8.1
//! title: using var u := move x.f is the canonical take-and-consume pattern
//! rule: "`using var u := move x.f;` is the canonical pattern for 'take the
//!   field's value out and consume it scope-bound.' The using var discharges
//!   the moved value at scope end; the field-restoration rule still applies
//!   at the surrounding method's exits."
//! cites: 28.8.1
//! expect: run
//! output: processed
//!   dispose first
//!   dispose second
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
    _resource := new Widget("first");
  end constructor;

  public method ProcessAndReplace()
    var fresh := new Widget("second");
    using var old := move _resource;
    _resource := fresh;
    Console.WriteLine("processed");
  end ProcessAndReplace;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.ProcessAndReplace();
  end Main;
end Program;
