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
    // r40: after the third lazy query over `a` is passed as an argument, `a` reads as disposed
    // (compiler bug); materialize each query first.
    using var u := a.Union(b).ToArray();
    using var i := a.Intersect(b).ToArray();
    using var x := a.Except(b).ToArray();
    Show("union", u);
    Show("both", i);
    Show("onlyA", x);
    var probe: Set<string> := ["api", "db"];
    Console.WriteLine($"subset: {probe.IsSubsetOf(a)}");
  end Main;
end Program;
