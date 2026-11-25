//! case: s14_1_9302
//! doc: spec
//! section: 14.1
//! title: an array of nullable generic records and a TryGetValue over generic records carry the payload
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 7.2
//! expect: run
//! output: 1051
//!   61
//!   71
//!   1051
//!   61
//!   71
//! notes: Harvested from the #9049 review (p5, second half). Trace per instantiation (V = int then V = string;
//!   the results do not depend on V): ArrNull adds two elements, Entry(.., 51) and null; the loop adds 51 for
//!   the non-null element and 1000 for the null one: 1051. DictNull stores Entry(.., 61) under "a" and null
//!   under "b" in a `Dictionary<string, Entry<V>?>`; `TryGetValue("a", out var t)` is true with t the stored
//!   entry, so the result is 61 + 0. TryGet stores Entry(.., 71) and `TryGetValue` is true, so it returns 71.
module Conf;
public record Entry<V>(Value: V, Expires: int);
public class Holder<V>
  public method ArrNull(v: V): int
    using var arr := new Array<Entry<V>?>();
    arr.Add(new Entry<V>(v, 51));
    arr.Add(null);
    var n := 0;
    for var x in arr do
      if x is not null then n := n + x!.Expires; else n := n + 1000; end if;
    end for;
    return n;
  end ArrNull;
  public method DictNull(v: V): int
    using var d := new Dictionary<string, Entry<V>?>();
    d["a"] := new Entry<V>(v, 61);
    d["b"] := null;
    var ok := d.TryGetValue("a", out var t);
    return (if t is not null then t!.Expires else -1) + (if ok then 0 else 1);
  end DictNull;
  public method TryGet(v: V): int
    using var d := new Dictionary<string, Entry<V>>();
    d["a"] := new Entry<V>(v, 71);
    var ok := d.TryGetValue("a", out var t);
    return (if ok then t!.Expires else -1);
  end TryGet;
end Holder;
public class Program
  public static method Main(): int
    using var h := new Holder<int>();
    Console.WriteLine(h.ArrNull(1));
    Console.WriteLine(h.DictNull(1));
    Console.WriteLine(h.TryGet(1));
    using var s := new Holder<string>();
    Console.WriteLine(s.ArrNull("z"));
    Console.WriteLine(s.DictNull("z"));
    Console.WriteLine(s.TryGet("z"));
    return 0;
  end Main;
end Program;
