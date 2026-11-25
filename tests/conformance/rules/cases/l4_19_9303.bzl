//! case: l4_19_9303
//! doc: stdlib
//! section: 4.19
//! title: a lambda reads Length of a captured FrozenArray of strings and of one of its elements
//! rule: public readonly property Length: int;
//! cites: 4.19, 27.3
//! expect: run
//! output: 2
//!   3
//!   héllo
//! notes: Harvested from the #9185 review (p2). Source ["héllo", "日本語"]. `f.Length` is the array's element
//!   count, 2. `f[1].Length` is the string's code-point count (stdlib 9.6), 3 for three CJK code
//!   points; the array's `Length` is an int and so is the string's. `f[0]` prints "héllo".
module Conf;
public class Program
  public static method Main(): int
    using var s := new Array<string>();
    s.Add("héllo"); s.Add("日本語");
    using var f := FrozenArray<string>.From(s);
    var fn := lambda() => f.Length;
    var g := lambda() => f[1].Length;
    Console.WriteLine(fn());
    Console.WriteLine(g());
    Console.WriteLine(f[0]);
    return 0;
  end Main;
end Program;
