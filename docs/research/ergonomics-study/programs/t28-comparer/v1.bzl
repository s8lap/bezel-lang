module Generic.Comparers;

record Person(Name: string, Age: int, City: string);

public static class Compare
  public static method By<T, K>(key: (T) -> K): (T, T) -> int where K: IComparable<K>
    return lambda(a, b) => key(a).CompareTo(key(b));
  end By;

  public static method Then<T>(first: (T, T) -> int, second: (T, T) -> int): (T, T) -> int
    return lambda(a, b) => do
      var c := first(a, b);
      return (if c <> 0 then c else second(a, b));
    end;
  end Then;

  public static method Reverse<T>(cmp: (T, T) -> int): (T, T) -> int
    return lambda(a, b) => cmp(b, a);
  end Reverse;

  public static method MaxBy<T, K>(xs: IEnumerable<T>, key: (T) -> K): T where K: IComparable<K>
    var best: T? := null;
    for var x in xs do
      if best = null or key(x).CompareTo(key(best)) > 0 then
        best := x;
      end;
    end;
    return best!;
  end MaxBy;
end Compare;

public class Program
  public static method Main()
    var people: Array<Person> := [
      new Person("ann", 30, "Oslo"), new Person("bob", 25, "Bergen"),
      new Person("cho", 40, "Oslo"), new Person("dee", 30, "Oslo"),
      new Person("eve", 25, "Bergen")
    ];
    var cmp := Compare.Then(Compare.By<Person, string>(lambda(p) => p.City),
                Compare.Then(Compare.Reverse(Compare.By<Person, int>(lambda(p) => p.Age)),
                             Compare.By<Person, string>(lambda(p) => p.Name)));
    people.Sort(cmp);
    for var p in people do
      Console.WriteLine($"{p.City} {p.Age} {p.Name}");
    end;
    Console.WriteLine($"oldest: {Compare.MaxBy(people, lambda(p) => p.Age).Name}");
  end Main;
end Program;
