//! case: s29_8_0024
//! doc: spec
//! section: 29.8
//! title: the same iterator over a plain borrowing parameter stays re-iterable
//! rule: the remedies are §29.11's — materialize first at a value element
//!   type, or take a class-typed resource as a plain (borrowing) parameter,
//!   which keeps the iterable re-iterable.
//! cites: 29.8, 29.11
//! expect: run
//! output: 1
//!   1
//!   closed
module Conf;

public class Lines
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Lines;

public class Program
  public static method Echo(source: Lines): owned IEnumerable<int>
    yield return 1;
  end Echo;

  public static method Main()
    using var src := new Lines();
    using var xs := Echo(src);
    for var a in xs do
      Console.WriteLine(a);
    end;
    for var b in xs do
      Console.WriteLine(b);
    end;
  end Main;
end Program;
