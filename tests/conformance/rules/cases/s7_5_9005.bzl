//! case: s7_5_9005
//! doc: spec
//! section: 7.5
//! title: documented members of several collection heads through `?.` chains are admitted
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 26.5
//! expect: run
//! output: 1
//!   0
//! notes: Harvested from the #8783 review (probes2/shapes.bzl), trimmed of the bare `Keys` read (#9162) and of
//!   the span/sort/reverse members. Each member is declared by the stdlib section for its head: Array
//!   Add/Count/Contains/IndexOf (4.4), Dictionary ContainsKey (4.8), Stack Push/Peek (4.11),
//!   SortedDictionary Add/FirstKey (4.17), LinkedList FirstNode (4.6), SortedSet Add/First (4.18),
//!   FrozenArray Length (4.19), OwningSet Count (4.10). Trace: Main prints the Stack peek (1), then Run
//!   prints n = 0 (Items is null, so the chain is null and `?? 0` gives 0).
module Conf;
public class Holder
  public property Items: Array<int>? := null;
  public property Map: Dictionary<string, int>? := null;
end Holder;
public class Program
  public static method Run(h: Holder?)
    h?.Items?.Add(1);
    var n := h?.Items?.Count ?? 0;
    var c := h?.Map?.ContainsKey("a") ?? false;
    if (h?.Items?.Contains(3) ?? false) then
      Console.WriteLine("yes");
    end if;
    var f := lambda(x: int) => h?.Items?.IndexOf(x) ?? -1;
    Console.WriteLine(n);
  end Run;
  public static method Main()
    var q: Stack<int>? := new Stack<int>();
    q?.Push(1);
    var p := q?.Peek() ?? 0;
    var dd: SortedDictionary<int, int>? := new SortedDictionary<int, int>();
    dd?.Add(2, 3);
    var fk := dd?.FirstKey ?? 0;
    var ll: LinkedList<int>? := new LinkedList<int>();
    var fn := ll?.FirstNode;
    var ss: SortedSet<int>? := new SortedSet<int>();
    ss?.Add(4);
    var ff := ss?.First ?? 0;
    var fz: FrozenArray<int>? := null;
    var len := fz?.Length ?? 0;
    var os: OwningSet<Holder>? := null;
    var oc := os?.Count ?? 0;
    Console.WriteLine(p);
    Run(new Holder());
  end Main;
end Program;
