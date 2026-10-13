//! case: s29_8_0023
//! doc: spec
//! section: 29.8
//! title: a bound iterable minted over an owned parameter enumerated twice is a compile error
//! rule: an iterable produced by an iterator method that declares an `owned`
//!   or move-only parameter ... is **single-consumption**, under exactly
//!   §29.11's rule for a lazy query ... a second consumption is a compile
//!   error where the flow analysis can see it
//! cites: 29.8, 29.11
//! expect: reject
//! diagnostic: ToArray
//! issue: 5824
module Conf;

public class Lines
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Lines;

public class Program
  public static method Echo(owned source: Lines): owned IEnumerable<int>
    yield return 1;
  end Echo;

  public static method Main()
    using var xs := Echo(new Lines());
    for var a in xs do
      Console.WriteLine(a);
    end;
    for var b in xs do
      Console.WriteLine(b);
    end;
  end Main;
end Program;
