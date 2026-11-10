module Query.Sets;

public class Program
  private static method Show(label: string, items: Array<string>)
    items.Sort();
    Console.WriteLine($"{label}: {String.Join(",", items)}");
  end Show;

  public static method Main()
    var a: Set<string> := ["web", "api", "db", "cache"];
    var b: Set<string> := ["db", "queue", "api", "auth"];
    // r40: v1-v3 raised DisposedAccessException (compiler bug, see compiler-bugs.md B13);
    // materializing every query before the helper call avoids it.
    var u := a.Union(b).ToArray();
    var i := a.Intersect(b).ToArray();
    var x := a.Except(b).ToArray();
    Show("union", u);
    Show("both", i);
    Show("onlyA", x);
    var probe: Set<string> := ["api", "db"];
    Console.WriteLine($"subset: {probe.IsSubsetOf(a)}");
  end Main;
end Program;
