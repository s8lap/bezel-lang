//! case: s14_1_9305
//! doc: spec
//! section: 14.1
//! title: control: concrete generic records and choices in a dictionary read back at their instantiations
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 4.8
//! expect: run
//! output: 3
//!   xx
//!   103
//!   s
//!   9
//!   zz
//! notes: Harvested from the #9049 review (ctl/c1), the non-generic-class control for s14_1_9301..4: the same
//!   generic record and choice, named at concrete arguments in a non-generic class. Trace: Entry<int>(1, 2):
//!   Value + Expires = 3. Entry<string>("xx", 2): Value "xx". Pick(true) is Entry(3, 4), so 3, Pick(false)
//!   is null so +100: 103. PickS(true).Value is "s". Opt<int>.Some(9) -> 9. Opt<string>.Some("zz") -> "zz".
module Conf;
public record Entry<V>(Value: V, Expires: int);
public choice Opt<V> Some(v: V), None; end Opt;
public class Program
  public static method Pick(f: bool): Entry<int>?
    if f then return new Entry<int>(3, 4); end if;
    return null;
  end Pick;
  public static method PickS(f: bool): Entry<string>?
    if f then return new Entry<string>("s", 4); end if;
    return null;
  end PickS;
  public static method Main(): int
    using var d := new Dictionary<string, Entry<int>>();
    d["a"] := new Entry<int>(1, 2);
    var e := d.GetOrNull("a");
    Console.WriteLine(e!.Value + e!.Expires);
    using var ds := new Dictionary<string, Entry<string>>();
    ds["a"] := new Entry<string>("xx", 2);
    var es := ds.GetOrNull("a");
    Console.WriteLine(es!.Value);
    var p := Pick(true);
    var q := Pick(false);
    Console.WriteLine((if p = null then 0 else p!.Value) + (if q = null then 100 else 0));
    var r := PickS(true);
    Console.WriteLine((if r = null then "n" else r!.Value));
    using var o := new Dictionary<string, Opt<int>>();
    o["a"] := Opt<int>.Some(9);
    var g := o.GetOrNull("a");
    Console.WriteLine((case g of Some(var x) => x, _ => -1));
    using var os := new Dictionary<string, Opt<string>>();
    os["a"] := Opt<string>.Some("zz");
    var gs := os.GetOrNull("a");
    Console.WriteLine((case gs of Some(var x) => x, _ => "none"));
    return 0;
  end Main;
end Program;
