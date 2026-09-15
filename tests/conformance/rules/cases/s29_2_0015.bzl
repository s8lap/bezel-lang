//! case: s29_2_0015
//! doc: spec
//! section: 29.2
//! title: yielding a named owned binding twice from a borrowing iterator is an admitted re-borrow
//! rule: in an iterator returning a **borrowing** enumeration interface the
//!   unmarked read is a **borrow** (nothing announces a kill) ... An
//!   unmarked read of a named owned binding in the yield position of a
//!   *borrowing* iterator ... is a re-borrow: the binding stays owned in
//!   the frame.
//! cites: 29.2
//! expect: run
//! stdout: 2
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Produce(): IEnumerable<Widget>
    var w := new Widget("x");
    yield return w;
    yield return w;
  end Produce;

  public static method Main()
    var total := 0;
    for var v in Produce() do
      total := total + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
