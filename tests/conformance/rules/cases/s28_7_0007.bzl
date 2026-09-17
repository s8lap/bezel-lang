//! case: s28_7_0007
//! doc: spec
//! section: 28.7
//! title: whole-value reassignment of an owning inline array disposes live slots in reverse order
//! rule: An owning inline array takes drop-on-overwrite per live slot, in
//!   reverse slot order (v0.260) — the RHS is evaluated first, then each
//!   live slot of the displaced value is disposed, then the new value is
//!   stored.
//! cites: 28.7, 15.3
//! expect: run
//! output: dispose b
//!   dispose a
//!   reassigned
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    if _name <> "" then
      Console.WriteLine($"dispose {_name}");
    end;
  end destructor;
end Widget;

public class Program
  public static method Main()
    var arr: [2 owned]Widget := [new Widget("a"), new Widget("b")];
    arr := [new Widget(""), new Widget("")];
    Console.WriteLine("reassigned");
  end Main;
end Program;
