//! case: s27_6_0023
//! doc: spec
//! section: 27.6
//! title: a bare reference to an undeclared member of an unannotated IList-typed local is rejected
//! rule: "the member resolved on the value exactly as the call `v.Scaled()` is" (§27.6, v0.340): `Nope` is not a member of
//!   `IList<T>` (stdlib §4.1: Count, IsReadOnly, Add, Clear, Contains, Remove, Insert, the indexer, and the inherited
//!   `GetEnumerator`), and no extension applies (§26.5), so `il.Nope` is rejected as `il.Nope()` is; the declared member
//!   `Count` on the same local is read without error.
//! cites: 26.5, 4.1
//! expect: reject
//! diagnostic: nope
module Conf;

public class Program
  public static method Mk(): IList<int>
    var a: Array<int> := [1, 2, 3];
    return a;
  end Mk;

  public static method Main()
    var il := Mk();
    Console.WriteLine(il.Count);
    var f := il.Nope;
  end Main;
end Program;
