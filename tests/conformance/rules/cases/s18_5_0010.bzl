//! case: s18_5_0010
//! doc: spec
//! section: 18.5
//! title: a null nullable hole with a format spec renders the padded text null, without invoking the spec
//! rule: "When the value is `null` at render time, the hole renders as the
//!   four-character text `null` ... A format spec on a nullable-typed hole is
//!   checked at compile time against the non-nullable form ... and the spec is
//!   simply not invoked when the value is null. The alignment applies to the
//!   rendered text `null` exactly as to any other rendered text." Worked example:
//!   `var x: double? := null; Console.WriteLine($"[{x,8:F2}]"); // prints:
//!   [    null]`.
//! cites: 18.5
//! expect: run
//! stdout: [    null]
module Conf;

public class Program
  public static method Main()
    var x: double? := null;
    Console.WriteLine($"[{x,8:F2}]");
  end Main;
end Program;
