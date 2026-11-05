//! case: l4_18_2402
//! doc: stdlib
//! section: 4.18
//! title: SortedSet<Guid>.First and .Last raise InvalidOperationException on an
//!   empty set
//! rule: least; InvalidOperationException if empty
//! cites: 4.18, 2.6, 4.4
//! expect: run
//! output: threw
//!   threw
//! notes: Red on base: the saved pre-fix compiler of the #8263 review refused
//!   `.First`/`.Last`/`.FirstKey`/`.LastKey` on a Guid element type with
//!   'requires the element type to implement IComparable<T>', although Guid is
//!   declared IComparable<Guid> (stdlib 2.6). No open issue describes this
//!   compile refusal (#8263 describes the iteration order). Both properties are
//!   declared 'InvalidOperationException if empty' in 4.18.
module M;
class C
  public static method Main(): int
    using var s := new SortedSet<Guid>();
    try
      Console.WriteLine(s.First);
    catch
      InvalidOperationException => Console.WriteLine("threw"),
    end try;
    try
      Console.WriteLine(s.Last);
    catch
      InvalidOperationException => Console.WriteLine("threw"),
    end try;
    return 0;
  end Main;
end C;
