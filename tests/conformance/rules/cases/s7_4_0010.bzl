//! case: s7_4_0010
//! doc: spec
//! section: 7.4
//! title: a mutably-captured local loses its narrowing at an intervening call
//! rule: A variable that is mutably captured — assigned anywhere inside any
//!   lambda of the enclosing function body — loses its narrowing once the
//!   controlled block performs any operation that can transfer control to
//!   user code, such as a method call.
//! cites: 27.5
//! expect: reject
//! diagnostic: mutably captured
module Conf;

public class Program
  public static method Noisy() => Console.WriteLine("noisy");

  public static method Main()
    var label: string? := "gizmo";
    var clear := lambda() => do label := null; end;
    if label <> null then
      Noisy();
      Console.WriteLine(label);
    end;
    clear();
  end Main;
end Program;
