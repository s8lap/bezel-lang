//! case: s28_8_1_0013
//! doc: spec
//! section: 28.8.1
//! title: detaching via a nullable field moves the contents out and restores to null
//! rule: A class that wants "take the resource away and leave the field
//!   empty" declares the field nullable and assigns `null` after the move —
//!   the reassignment to null satisfies the field-restoration rule.
//! cites: 28.8.1
//! expect: run
//! output: detached
//!   dispose first
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
  private owned _resource: Widget?;

  public constructor()
    _resource := new Widget("first");
  end constructor;

  public method Detach(): owned Widget?
    if _resource = null then return null; end;
    var taken := move _resource!;
    _resource := null;
    return taken;
  end Detach;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    using var taken := h.Detach();
    Console.WriteLine("detached");
  end Main;
end Program;
