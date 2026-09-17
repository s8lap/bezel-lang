//! case: s7_1_0001
//! doc: spec
//! section: 7.1
//! title: a non-nullable reference implicitly converts to its nullable form
//! rule: A non-nullable reference type implicitly converts to its nullable
//!   form.
//! expect: run
//! stdout: present
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w: Widget := new Widget();
    var maybe: Widget? := w;
    if maybe <> null then
      Console.WriteLine("present");
    else
      Console.WriteLine("absent");
    end;
  end Main;
end Program;
