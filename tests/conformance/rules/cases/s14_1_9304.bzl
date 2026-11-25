//! case: s14_1_9304
//! doc: spec
//! section: 14.1
//! title: a generic method in a generic class reads generic-record dictionary values at both parameter lists
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 14.5
//! expect: run
//! output: 45
//!   45
//!   45
//! notes: Harvested from the #9049 review (p7). `Map<U>` builds one dictionary of `Entry<U>` (Expires 4) and
//!   one of `Entry<T>` (Expires 5) and reads each back with `GetOrNull`: 4 * 10 + 5 = 45 at every
//!   (T, U): (int, string), (int, int) and (string, Entry<int>).
module Conf;
public record Entry<V>(Value: V, Expires: int);
public class Box<T>
  public method Map<U>(t: T, u: U): int
    using var d := new Dictionary<string, Entry<U>>();
    d["a"] := new Entry<U>(u, 4);
    var e := d.GetOrNull("a");
    using var d2 := new Dictionary<string, Entry<T>>();
    d2["a"] := new Entry<T>(t, 5);
    var e2 := d2.GetOrNull("a");
    return e!.Expires * 10 + e2!.Expires;
  end Map;
end Box;
public class Program
  public static method Main(): int
    using var b := new Box<int>();
    Console.WriteLine(b.Map<string>(1, "x"));
    Console.WriteLine(b.Map<int>(1, 2));
    using var c := new Box<string>();
    Console.WriteLine(c.Map<Entry<int>>("a", new Entry<int>(1, 2)));
    return 0;
  end Main;
end Program;
