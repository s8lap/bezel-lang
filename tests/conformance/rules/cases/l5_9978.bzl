//! case: l5_9978
//! doc: stdlib
//! section: 5
//! title: bare instance method names passed to TakeWhile and SkipWhile split at the first failing element
//! rule: is **retained by the returned query value until enumeration**, not just
//!   for the duration of the call
//! cites: 5, 27.6, 2.7
//! expect: run
//! issue: 9292
//! stdout: 23 14
module Conf;

public class Program
  public method Keep(x: int): bool => x > 1;

  public method Take(): string
    using var xs: Array<int> := [2, 3, 1, 4];
    var s := "";
    for var y in xs.TakeWhile(Keep) do
      s := s + y.ToString();
    end;
    return s;
  end Take;

  public method Skip(): string
    using var xs: Array<int> := [2, 3, 1, 4];
    var s := "";
    for var y in xs.SkipWhile(Keep) do
      s := s + y.ToString();
    end;
    return s;
  end Skip;

  public static method Main()
    var taken := "";
    var skipped := "";
    for var j in 0 ..< 100 do
      using var p := new Program();
      taken := p.Take();
      skipped := p.Skip();
    end;
    Console.WriteLine($"{taken} {skipped}");
  end Main;
end Program;
