//! case: s28_3_0043
//! doc: spec
//! section: 28.3
//! title: an unbound owned temporary is disposed at the end of the full expression
//! rule: An unbound owned temporary is disposed at the end of the full expression in
//!   which it is produced, after every borrowing use within that expression.
//! cites: C.3
//! expect: run
//! output: widget temp
//!   dispose temp
module Conf;

public class Widget
  private _label: string;

  public constructor(label: string)
    _label := label;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_label}");
  end destructor;

  public method Describe(): string => $"widget {_label}";
end Widget;

public class Program
  public static method Main()
    Console.WriteLine(new Widget("temp").Describe());
  end Main;
end Program;
