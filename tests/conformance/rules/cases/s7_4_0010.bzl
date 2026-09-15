//! case: s7_4_0010
//! doc: spec
//! section: 7.4
//! title: a mutably-captured local loses its narrowing at an intervening call
//! rule: A variable that is mutably captured by any lambda of the enclosing
//!   function, and the block performs any operation that can transfer
//!   control to user code, loses its narrowing after that operation.
//! cites: 27.5
//! expect: reject
//! diagnostic: mutably captured
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Program
  public static method Noisy() => Console.WriteLine("noisy");

  public static method Main()
    var w: Widget? := new Widget();
    var clear := lambda() => do w := null; end;
    if w <> null then
      Noisy();
      Console.WriteLine(w.Name);
    end;
    clear();
  end Main;
end Program;
