module Generic.Comparers;

record Person(Name: string, Age: int, City: string);

// A comparer combinator cannot return a closure over its function parameters (§28.14:
// a plain function-typed parameter is call-bounded; `scoped` on a static method needs an
// owned return, which a function type cannot have). Write the composite comparer inline.
public static class Compare
  public static method MaxBy<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then
        best := x;
      end;
    end;
    return best;
  end MaxBy;
end Compare;

public class Program
  public static method Main()
    var people: Array<Person> := [
      new Person("ann", 30, "Oslo"), new Person("bob", 25, "Bergen"),
      new Person("cho", 40, "Oslo"), new Person("dee", 30, "Oslo"),
      new Person("eve", 25, "Bergen")
    ];
    people.Sort(lambda(a: Person, b: Person): int => do
      var c := a.City.CompareTo(b.City);
      if c <> 0 then return c; end;
      c := b.Age.CompareTo(a.Age);
      if c <> 0 then return c; end;
      return a.Name.CompareTo(b.Name);
    end);
    for var p in people do
      Console.WriteLine($"{p.City} {p.Age} {p.Name}");
    end;
    var oldest := Compare.MaxBy<Person, int>(people, lambda(p) => p.Age);
    Console.WriteLine($"oldest: {oldest.Name}");
  end Main;
end Program;
