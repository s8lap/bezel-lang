//! case: s22_6_0206
//! doc: spec
//! section: 22.6
//! title: this passed to string.StartsWith directly in a class member is rejected
//! rule: If no candidate is applicable at all — no candidate is reachable: a conversion §5.1 does not
//!   admit as implicit
//! cites: 5.1, 12.3
//! expect: reject
//! diagnostic: §5.1
//! issue: 8130
module Conf;

public class C
  public N: int := 1;

  public method Go()
    var b := "abc".StartsWith(this);
  end Go;
end C;

public class Program
  public static method Main()
    using var c := new C();
    Console.WriteLine(c.N);
    c.Go();
  end Main;
end Program;
