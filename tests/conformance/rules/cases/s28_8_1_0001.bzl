//! case: s28_8_1_0001
//! doc: spec
//! section: 28.8.1
//! title: move on a field takes it out and a restoring assignment on every exit path satisfies the rule
//! rule: The field-restoration rule: every control-flow path from the move
//!   site to the method's exit must execute a restoring assignment
//!   `x.f := value;` between the move and the exit.
//! cites: 28.8.1
//! expect: run
//! stdout: taken first
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  public readonly property Name: string => _name;
end Widget;

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget("first");
  end constructor;

  public method ReplaceAndReturn(): owned Widget
    var fresh := new Widget("second");
    var old := move _resource;
    _resource := fresh;
    return old;
  end ReplaceAndReturn;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    using var taken := h.ReplaceAndReturn();
    Console.WriteLine($"taken {taken.Name}");
  end Main;
end Program;
