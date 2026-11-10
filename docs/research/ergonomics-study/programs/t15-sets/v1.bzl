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
    Show("union", a.Union(b));
    Show("both", a.Intersect(b));
    Show("onlyA", a.Except(b));
    var probe: Set<string> := ["api", "db"];
    Console.WriteLine($"subset: {probe.IsSubsetOf(a)}");
  end Main;
end Program;
