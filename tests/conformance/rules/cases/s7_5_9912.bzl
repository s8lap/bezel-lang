//! case: s7_5_9912
//! doc: spec
//! section: 7.5
//! title: The ?. spelling performs an extension member over an enum receiver
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 26.5
//! expect: run
//! output: 20
//!   green
//!   30
//!   0
//! notes: Harvested from the #9243 review (r43-rev-m146-9243). A value-shaped receiver (an int or an enum)
//!   is non-null here, so `x?.M` performs the access, and an extension member is found by 26.5 rule 2
//!   exactly as for `.`. The result type stays nullable (7.5). The record-receiver and Console.Out
//!   residuals of #9243 are not covered here. Green: 20 and green; a Blue Color?: 30; a null Color?:
//!   the access does not occur, the fallback 0 prints.
module Conf;

enum Color Red, Green, Blue; end Color;

public extension Color
  public method Code(): int => (case this of Color.Red => 10, Color.Green => 20, Color.Blue => 30, _ => -1);
  public method Name(): string => (case this of Color.Red => "red", Color.Green => "green", _ => "other");
end;

public class Program
  public static method Main()
    var c := Color.Green;
    var a := c?.Code();
    var b := c?.Name();
    Console.WriteLine((a ?? -1).ToString());
    Console.WriteLine(b ?? "null");
    var e: Color? := Color.Blue;
    Console.WriteLine(((e?.Code()) ?? -1).ToString());
    var n: Color? := null;
    var m := n?.Code();
    Console.WriteLine((m ?? 0).ToString());
  end Main;
end Program;
