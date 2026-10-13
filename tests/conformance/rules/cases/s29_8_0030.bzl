//! case: s29_8_0030
//! doc: spec
//! section: 29.8
//! title: the same generic iterator at a copyable instantiation is re-iterable
//! rule: so `Each<T>(x: T)` mints a single-consumption iterable at
//!   `Each<Slot>` for an owning choice `Slot` and a re-iterable one at
//!   `Each<int>`
//! cites: 29.8, 29.11, 15.3
//! expect: run
//! output: 1
//!   1
module Conf;

public class Program
  public static method Each<T>(x: T): owned IEnumerable<int>
    yield return 1;
  end Each;

  public static method Main()
    using var xs := Each<int>(5);
    for var a in xs do
      Console.WriteLine(a);
    end;
    for var b in xs do
      Console.WriteLine(b);
    end;
  end Main;
end Program;
