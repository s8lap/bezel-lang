module Query.Sets;

public class Program
  private static method Show(label: string, items: Array<string>)
    items.Sort();
    Console.WriteLine($"{label}: {String.Join(",", items)}");
  end Show;

  public static method Main()
    var a: Set<string> := ["web", "api", "db", "cache"];
    var b: Set<string> := ["db", "queue", "api", "auth"];
    // r40: v1-v4 raised DisposedAccessException at the subset test (compiler bug B13).
    // Testing membership element by element avoids Set.IsSubsetOf.
    var probe: Array<string> := ["api", "db"];
    var subset := probe.All(lambda(t) => a.Contains(t));
    var u := a.Union(b).ToArray();
    var i := a.Intersect(b).ToArray();
    var x := a.Except(b).ToArray();
    Show("union", u);
    Show("both", i);
    Show("onlyA", x);
    Console.WriteLine($"subset: {subset}");
  end Main;
end Program;
