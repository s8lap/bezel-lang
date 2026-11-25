//! case: s7_5_9910
//! doc: spec
//! section: 7.5
//! title: The ?. spelling performs an extension member over an int receiver
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 26.5
//! expect: run
//! output: 42
//!   26
//!   n=21
//!   21
//!   42
//!   n=21
//! notes: Harvested from the #9243 review (r43-rev-m146-9243). A value-shaped receiver (an int or an enum)
//!   is non-null here, so `x?.M` performs the access, and an extension member is found by 26.5 rule 2
//!   exactly as for `.`. The result type stays nullable (7.5). The record-receiver and Console.Out
//!   residuals of #9243 are not covered here. x = 21: Dbl 42, Plus(5) 26, Str n=21, Self 21.
module Conf;

public extension int
  public method Dbl(): int => this * 2;
  public method Plus(n: int): int => this + n;
  public method Str(): string => "n=" + this.ToString();
  public method Self(): int => this;
end;

public class Program
  public static method Main()
    var x: int := 21;
    var a := x?.Dbl();
    var b := x?.Plus(5);
    var s := x?.Str();
    Console.WriteLine((a ?? -1).ToString());
    Console.WriteLine((b ?? -1).ToString());
    Console.WriteLine(s ?? "null");
    Console.WriteLine(((x?.Self()) ?? -1).ToString());
    var q := (x?.Dbl()) ?? 0;
    Console.WriteLine(q.ToString());
    var t := (x?.Str()) ?? "none";
    Console.WriteLine(t);
  end Main;
end Program;
