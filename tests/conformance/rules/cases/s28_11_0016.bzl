//! case: s28_11_0016
//! doc: spec
//! section: 28.11
//! title: a direct FromCollectionLiteral copy into a local is admitted
//! rule: An inline array's `ToArray` (§15.1), range slicing (§15.6) and a direct
//!   `FromCollectionLiteral` call are shape (i)'s copies ... The destination is
//!   where the result lands (its binding, an unbound temporary's full expression,
//!   §28.3, or the field, collection or return slot it is placed in)
//! cites: 28.11, 9.7, 15.4.1
//! expect: run
//! stdout: 1
//! issue: 5721
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public record Pair(Ref: Item);

public class Program
  public static method Main()
    using var it := new Item();
    var xs: Array<Pair> := [new Pair(it)];
    using var ys := Array<Pair>.FromCollectionLiteral(xs.AsReadOnlySpan());
    Console.WriteLine(ys[0].Ref.N().ToString());
  end Main;
end Program;
