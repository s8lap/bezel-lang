module Query.Sets;

public class Program
  private static method Show(label: string, items: Array<string>)
    items.Sort();
    Console.WriteLine($"{label}: {String.Join(",", items)}");
  end Show;

  private static method SetOf(items: Array<string>): owned Set<string>
    var s := new Set<string>();
    for var t in items do
      s.Add(t);
    end;
    return s;
  end SetOf;

  public static method Main()
    // r40: v1-v5 raised DisposedAccessException (compiler bug B13); building the sets with
    // Add instead of set literals, and using the in-place set operations, avoids it.
    using var a := SetOf(["web", "api", "db", "cache"]);
    using var b := SetOf(["db", "queue", "api", "auth"]);
    using var u := SetOf(a.ToArray());
    u.UnionWith(b);
    using var i := SetOf(a.ToArray());
    i.IntersectWith(b);
    using var x := SetOf(a.ToArray());
    x.ExceptWith(b);
    Show("union", u.ToArray());
    Show("both", i.ToArray());
    Show("onlyA", x.ToArray());
    using var probe := SetOf(["api", "db"]);
    Console.WriteLine($"subset: {probe.IsSubsetOf(a)}");
  end Main;
end Program;
