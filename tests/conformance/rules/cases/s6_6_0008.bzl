//! case: s6_6_0008
//! doc: spec
//! section: 6.6
//! title: after ??= the target is narrowed non-null and admits the ordinary compound form
//! rule: ??= is unaffected — it is not a read-modify-write, and the absent case
//!   is its whole purpose; after x ??= e; the target is narrowed non-null
//!   (§7.4), so the ordinary compound form applies to it from the next
//!   statement on.
//! cites: 6.6, 7.4, 7.7
//! expect: run
//! stdout: 6
module Conf;

public class Program
  public static method Main()
    var n: int? := null;
    n ??= 5;
    n += 1;
    Console.WriteLine($"{n}");
  end Main;
end Program;
