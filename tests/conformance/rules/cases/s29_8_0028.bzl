//! case: s29_8_0028
//! doc: spec
//! section: 29.8
//! title: two GetEnumerator calls on an iterable over a plain parameter produce independent enumerators
//! rule: the remedies are §29.11's — materialize first at a value element
//!   type, or take a class-typed resource as a plain (borrowing) parameter,
//!   which keeps the iterable re-iterable.
//! cites: 29.8, 29.11
//! expect: run
//! output: 2
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
    using var e1 := xs.GetEnumerator();
    using var e2 := xs.GetEnumerator();
    _ := e1.MoveNext();
    _ := e2.MoveNext();
    Console.WriteLine(e1.Current + e2.Current);
  end Main;
end Program;
