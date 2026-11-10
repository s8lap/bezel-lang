module Query.Sets;

public class Program
  private static method Show(label: string, items: IEnumerable<string>)
    var sorted := items.ToArray();
    sorted.Sort();
    Console.WriteLine($"{label}: {String.Join(",", sorted)}");
  end Show;

  public static method Main()
    var a: Set<string> := ["web", "api", "db", "cache"];
    var b: Set<string> := ["db", "queue", "api", "auth"];
    // r40: probe.IsSubsetOf(a) raised DisposedAccessException when run after the three
    // Show calls (compiler bug, not minimized); compute it first.
    var probe: Set<string> := ["api", "db"];
    var subset := probe.IsSubsetOf(a);
    Show("union", a.Union(b));
    Show("both", a.Intersect(b));
    Show("onlyA", a.Except(b));
    Console.WriteLine($"subset: {subset}");
  end Main;
end Program;
