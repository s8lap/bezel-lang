//! case: s11_7_9860
//! doc: spec
//! section: 11.7
//! title: ReferenceEquals through an alias of a static class is the inherited root static
//! rule: is inherited by a static class as by every class
//! cites: 11.7, 8.1, 27.8
//! expect: run
//! output: True
//!   1
//! issue: 9021
//! notes: Harvested from the #9021 review (q08_alias_static_class). `SA` is an alias of the static class
//!   `S`; 27.8 sees through it, 11.7 has the root's static inherited by the static class, so
//!   `SA.ReferenceEquals(k, k)` answers True, and the alias still reaches the class's own static
//!   method (`SA.F()` is 1).
module Conf;

public static class S
  public static method F(): int => 1;
end S;

public class K end K;
type SA := S;

public class Program
  public static method Main(): int
    var k := new K();
    Console.WriteLine(SA.ReferenceEquals(k, k));
    Console.WriteLine(SA.F());
    return 0;
  end Main;
end Program;
