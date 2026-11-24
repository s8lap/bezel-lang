//! case: s28_3_8001
//! doc: spec
//! section: 28.3
//! title: a nested lambda reading an enclosing lambda's owning choice local by case disposes
//!   it once
//! rule: is uniformly in **borrow state**: reads borrow
//! cites: 28.3, 28.9, 19.3
//! expect: run
//! stdout: 200 2800
//! notes: Pins #8987's admitting control: reads of the captured owned choice borrow (a
//!   `case` peek is a read), so the new rejection must not fire.
module Conf;

public class Res
  public static Count: int := 0;
  public N: int := 7;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;

public class Program
  public static method Step(): int
    var outer := lambda(): int => do
      var o: OC? := OC.Text(new Res());
      var g := lambda(): int => do
        var k := 0;
        case o of Text(var r) => k := r.N, _ => k := 0 end;
        return k;
      end;
      return g() + g();
    end;
    return outer();
  end Step;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do s := s + Step(); end;
    Console.WriteLine($"{Res.Count} {s}");
    return 0;
  end Main;
end Program;
