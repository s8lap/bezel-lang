//! case: l4_17_2402
//! doc: stdlib
//! section: 4.17
//! title: SortedDictionary<Guid, int>.FirstKey and .LastKey raise
//!   InvalidOperationException on an empty map
//! rule: least key; InvalidOperationException if empty
//! cites: 4.17, 2.6
//! expect: run
//! output: threw
//!   threw
//! notes: Red on base: the saved pre-fix compiler of the #8263 review refused
//!   `.First`/`.Last`/`.FirstKey`/`.LastKey` on a Guid element type with
//!   'requires the element type to implement IComparable<T>', although Guid is
//!   declared IComparable<Guid> (stdlib 2.6). No open issue describes this
//!   compile refusal (#8263 describes the iteration order).
module M;
class C
  public static method Main(): int
    using var d := new SortedDictionary<Guid, int>();
    try
      Console.WriteLine(d.FirstKey);
    catch
      InvalidOperationException => Console.WriteLine("threw"),
    end try;
    try
      Console.WriteLine(d.LastKey);
    catch
      InvalidOperationException => Console.WriteLine("threw"),
    end try;
    return 0;
  end Main;
end C;
