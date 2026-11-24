//! case: s28_3_8005
//! doc: spec
//! section: 28.3
//! title: a nested lambda copying an enclosing lambda's copyable value choice local is
//!   admitted and matched
//! rule: copies for an ordinary value type; moves for a move-only value type
//! cites: 15.3, 28.3
//! expect: run
//! stdout: 3
//! notes: Pins #8987's over-rejection guard: a value-like choice is copyable (15.3), not an
//!   owned binding, so the closure copying it is admitted.
module Conf;
public choice VC Num(n: int), Nothing; end VC;
public class Program
  public static method Main(): int
    var outer := lambda(): int => do
      var o := VC.Num(3);
      var g := lambda(): int => do
        var x := o;
        var k := 0;
        case x of Num(var n) => k := n, _ => k := 0 end;
        return k;
      end;
      return g();
    end;
    Console.WriteLine(outer());
    return 0;
  end Main;
end Program;
