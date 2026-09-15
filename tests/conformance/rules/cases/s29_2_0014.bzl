//! case: s29_2_0014
//! doc: spec
//! section: 29.2
//! title: a named owned binding yielded from an owning iterator is a marker-free consume
//! rule: Under the marker discipline the operand read of a **named owned
//!   binding** is classified by the return type: in an iterator returning
//!   an **owning** enumeration interface the read is a marker-free
//!   owned-consuming position — the declared return type is the
//!   announcement (K3.13), exactly as for `return` under an `owned` return
//!   type.
//! cites: 29.2
//! expect: run
//! stdout: x
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Produce(): IOwningEnumerable<Widget>
    var w := new Widget("x");
    yield return w;
  end Produce;

  public static method Main()
    for var x in Produce() do
      Console.WriteLine(x.Name);
    end;
  end Main;
end Program;
