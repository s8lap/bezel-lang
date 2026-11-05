//! case: l2_2_2008
//! doc: stdlib
//! section: 2.2
//! title: Math.Min and Math.Max over Guid accept record-property, call and
//!   nested-call operands
//! rule: `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//!   otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and
//!   `b` otherwise
//! cites: 2.6
//! expect: run
//! output: 44444444-4444-4444-4444-444444444444
//!   22222222-2222-2222-2222-222222222222
//!   22222222-2222-2222-2222-222222222222
//!   True
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public record Box(G: Guid);

public class Program
  public static method Pick(): Guid
    return Guid.Parse("33333333-3333-3333-3333-333333333333");
  end Pick;

  public static method Main()
    var arr: Array<Guid> := [Guid.Parse("11111111-1111-1111-1111-111111111111"), Guid.Parse("22222222-2222-2222-2222-222222222222")];
    var bx := new Box(Guid.Parse("44444444-4444-4444-4444-444444444444"));
    Console.WriteLine(Math.Max(Pick(), bx.G).ToString());
    Console.WriteLine(Math.Min(Math.Max(arr[0], arr[1]), Pick()).ToString());
    var nb := new Box(Math.Min(bx.G, arr[1]));
    Console.WriteLine(nb.G.ToString());
    Console.WriteLine(Math.Max(arr[0], arr[1]) = arr[1]);
  end Main;
end Program;
