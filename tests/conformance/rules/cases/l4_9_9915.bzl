//! case: l4_9_9915
//! doc: stdlib
//! section: 4.9
//! title: Keys and Values of an OwningDictionary keep the query operators
//! rule: public readonly property Keys: IReadOnlyCollection<K>;
//! cites: 4.9, 5, 4.1
//! expect: run
//! output: a
//!   a
//!   1
//!   1
//!   1
//! notes: Harvested from the #9285 review (A03_keys_values). The twin of the l4_9_9910 family: the refusal
//!   is of the query operators on the OwningDictionary itself; its Keys and Values are
//!   IReadOnlyCollection views (a borrowing view, stdlib 4.9) and keep every operator.
module Conf;

public class Box
  public V: int := 1;
end Box;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Box>();
    od["a"] := new Box();
    for var k in od.Keys.Where(lambda(s) => true) do Console.WriteLine(k); end;
    for var k in od.Keys.Take(1) do Console.WriteLine(k); end;
    for var k in od.Keys.Select(lambda(s) => s.Length) do Console.WriteLine(k); end;
    for var b in od.Values.Take(1) do Console.WriteLine(b.V); end;
    for var b in od.Values.Skip(0) do Console.WriteLine(b.V); end;
  end Main;
end Program;
