//! case: s9_7_0039
//! doc: spec
//! section: 9.7
//! title: non-owning inline array of interface in instance field is not a span
//! rule: A non-owning inline array ([N]T, [N]T?) is an ordinary value type. Its
//!   element type being a class or interface does not make the array borrow-bearing
//!   or a span. A span may not be stored in an instance field (§9.7); an inline
//!   array of a borrow-carrying element type may — it is not a span.
//! cites: 8.6, 9.7, 15.1
//! expect: run
//! stdout: 5
//! issue: 5662
module M;

interface IHolder
  method GetV(): string;
end IHolder;

record RecHolder(V: string): IHolder
  public method GetV(): string => V;
end RecHolder;

class Holder
  public Items: [1]IHolder := [new RecHolder("hello")];
end Holder;

public class C
  public static method Main()
    var h: Holder := new Holder();
    Console.WriteLine(h.Items[0].GetV().Length);
  end Main;
end C;