//! case: s21_10_0001
//! doc: spec
//! section: 21.10
//! title: a local record is declared and used inside its enclosing body
//! rule: A `record` or `enum` may be declared as a statement, scoped to
//!   the enclosing body like a local variable. The local type is an
//!   ordinary nominal record.
//! cites: 21.10
//! expect: run
//! stdout: ab:2 xyz:3
module Conf;

public class Program
  public static method Render(names: Array<string>): string
    record Tagged(Name: string, Len: int);
    var sb := new StringBuilder();
    for var n in names do
      var t := new Tagged(n, n.Length);
      sb.Append($"{t.Name}:{t.Len}");
      sb.Append(" ");
    end;
    return sb.ToString();
  end Render;

  public static method Main()
    var names: Array<string> := ["ab", "xyz"];
    var rendered := Render(names);
    Console.WriteLine(rendered.Trim());
  end Main;
end Program;
