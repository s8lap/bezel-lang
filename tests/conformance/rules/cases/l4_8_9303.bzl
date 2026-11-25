//! case: l4_8_9303
//! doc: stdlib
//! section: 4.8
//! title: control: GetOrNull over a non-generic record and choice value reads back the stored payload
//! rule: `null` when the key is absent, otherwise the value under the family's read semantics
//! cites: 4.8, 35.4
//! expect: run
//! output: seven
//!   7
//!   four
//!   True
//! notes: Harvested from the #9049 review (p4 ctl), the non-generic twin of l4_8_9301/2. Trace: "a" holds
//!   Entry(7, "seven", 3); `e` is that value: prints its Name "seven" and Value 7. "a" in the choice
//!   dictionary holds Some(4, "four"); the case yields the string payload "four". `f` looks up the absent key
//!   "b": null, so `f is null` renders True (spec 8.5).
module Conf;
public record Entry(Value: int, Name: string, Expires: int);
public choice Opt Some(v: int, s: string), None; end Opt;
public class Program
  public static method Main(): int
    using var d := new Dictionary<string, Entry>();
    d["a"] := new Entry(7, "seven", 3);
    var e := d.GetOrNull("a");
    var f := d.GetOrNull("b");
    Console.WriteLine(e!.Name);
    Console.WriteLine(e!.Value);
    using var o := new Dictionary<string, Opt>();
    o["a"] := Opt.Some(4, "four");
    var g := o.GetOrNull("a");
    Console.WriteLine((case g of Some(var x, var s) => s, _ => "none"));
    Console.WriteLine(f is null);
    return 0;
  end Main;
end Program;
