//! case: l4_8_9302
//! doc: stdlib
//! section: 4.8
//! title: GetOrNull over a generic choice value distinguishes present, present-None and absent
//! rule: `null` when the key is absent, otherwise the value under the family's read semantics
//! cites: 4.8, 14.1, 35.4
//! expect: run
//! output: 11001
//!   42
//!   11001
//!   hello
//! notes: Harvested from the #9049 review (p3). Trace of Get: the dictionary holds "a" -> Some(v) and "n" ->
//!   None. `o` is the Some value, so its case adds 1; `n` is the stored None value (present, so not null),
//!   which matches `_` in its case and adds 1000; `m` is the lookup of an absent key, null, which matches
//!   `_` and adds 10000: 11001 at every V. Val stores Some(v) and returns the payload bound by the arm, which
//!   is v: 42 at int, "hello" at string.
module Conf;
public choice Opt<V> Some(v: V), None; end Opt;
public class Cache<V>
  public method Get(k: string, v: V): int
    using var d := new Dictionary<string, Opt<V>>();
    d[k] := Opt<V>.Some(v);
    d["n"] := Opt<V>.None;
    var o := d.GetOrNull(k);
    var n := d.GetOrNull("n");
    var m := d.GetOrNull("missing");
    var r := 0;
    r := r + (case o of Some(var x) => 1, _ => 10);
    r := r + (case n of Some(var x) => 100, _ => 1000);
    r := r + (case m of Some(var x) => 0, _ => 10000);
    return r;
  end Get;
  public method Val(k: string, v: V): V
    using var d := new Dictionary<string, Opt<V>>();
    d[k] := Opt<V>.Some(v);
    var o := d.GetOrNull(k);
    return (case o of Some(var x) => x, _ => v);
  end Val;
end Cache;
public class Program
  public static method Main(): int
    using var a := new Cache<int>();
    Console.WriteLine(a.Get("a", 42));
    Console.WriteLine(a.Val("a", 42));
    using var b := new Cache<string>();
    Console.WriteLine(b.Get("a", "x"));
    Console.WriteLine(b.Val("a", "hello"));
    return 0;
  end Main;
end Program;
