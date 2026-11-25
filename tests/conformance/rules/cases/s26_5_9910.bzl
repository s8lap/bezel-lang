//! case: s26_5_9910
//! doc: spec
//! section: 26.5
//! title: A user extension over OwningDictionary supplies the names the stdlib does not
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the
//!   current module and all imported modules
//! cites: 26.5, 4.9
//! expect: run
//! output: 41
//!   52
//!   77
//!   91
//!   41
//!   52
//!   77
//! notes: Harvested from the #9285 review (A05d_user_ext_many). OwningDictionary declares no instance
//!   member of these names (stdlib 4.9: IDisposable only), so 26.5 rule 2 searches the user
//!   extension: the refusal of the stdlib query operators (l4_9_9910 family) must not shadow a user
//!   extension of the same name, in the `.`, `?.` and `!.` spellings.
module Conf;

public class Box
  public V: int := 1;
end Box;

extension OwningDictionary<K, V> where V: class
  public method Take(n: int): int => n + 40;
  public method Where(n: int): int => n + 50;
  public method Distinct(): int => 77;
  public method OrderBy(n: int): int => n + 90;
end;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Box>();
    od["a"] := new Box();
    var od2: OwningDictionary<string, Box>? := od;
    Console.WriteLine(od.Take(1));
    Console.WriteLine(od.Where(2));
    Console.WriteLine(od.Distinct());
    Console.WriteLine(od.OrderBy(1));
    Console.WriteLine(od2?.Take(1) ?? -1);
    Console.WriteLine(od2?.Where(2) ?? -1);
    Console.WriteLine(od2!.Distinct());
  end Main;
end Program;
