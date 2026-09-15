//! case: s29_4_0002
//! doc: spec
//! section: 29.4
//! title: an iterator's plain reference-typed parameter is held as a borrow across yields
//! rule: The returned state machine holds the factory's plain
//!   reference-typed arguments as borrows until it completes or is disposed.
//! cites: 29.4
//! expect: run
//! stdout: aa
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Twice(w: Widget): IEnumerable<string>
    yield return w.Name;
    yield return w.Name;
  end Twice;

  public static method Main()
    var w := new Widget("a");
    var sb := new StringBuilder();
    for var s in Twice(w) do
      sb.Append(s);
    end;
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
