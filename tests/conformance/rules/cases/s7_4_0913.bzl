//! case: s7_4_0913
//! doc: spec
//! section: 7.4
//! title: a ReadOnlyStringSpan? mutably captured by a lambda loses its narrowing after a function-value
//!   call
//! rule: the variable is **mutably captured** — assigned, or passed as `ref`/`out`, anywhere inside any
//!   lambda of the enclosing function body
//! cites: 7.4
//! expect: reject
//! diagnostic: mutably
module Conf;

public class Program
  public static method Main()
    var w := "hello";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 0, 5);
    var f := lambda() => do
      s := null;
    end;
    if s <> null then
      f();
      Console.WriteLine(s.Length);
    end if;
  end Main;
end Program;
