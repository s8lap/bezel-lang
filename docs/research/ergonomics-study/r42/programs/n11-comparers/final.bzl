module People.Sorting;

public record Person(Name: string, Age: int, City: string);

public interface IOrder<T>
  method Compare(a: T, b: T): int;
end IOrder;

public class ByCity: IOrder<Person>
  public method Compare(a: Person, b: Person): int => a.City.CompareTo(b.City);
end ByCity;

public class ByAgeDesc: IOrder<Person>
  public method Compare(a: Person, b: Person): int => b.Age.CompareTo(a.Age);
end ByAgeDesc;

public class ByName: IOrder<Person>
  public method Compare(a: Person, b: Person): int => a.Name.CompareTo(b.Name);
end ByName;

public class ThenOrder<T>: IOrder<T>
  private _first: IOrder<T>;
  private _second: IOrder<T>;

  public constructor(first: IOrder<T>, second: IOrder<T>)
    _first := first;
    _second := second;
  end constructor;

  public method Compare(a: T, b: T): int
    var c := _first.Compare(a, b);
    return (if c <> 0 then c else _second.Compare(a, b));
  end Compare;
end ThenOrder;

public class Program
  public static method Main()
    var people: Array<Person> := [
      new Person("ann", 30, "Oslo"), new Person("bob", 25, "Bergen"), new Person("cho", 40, "Oslo"),
      new Person("dee", 30, "Oslo"), new Person("eve", 25, "Bergen")
    ];
    using var byCity := new ByCity();
    using var byAge := new ByAgeDesc();
    using var byName := new ByName();
    using var ageThenName := new ThenOrder<Person>(byAge, byName);
    using var order := new ThenOrder<Person>(byCity, ageThenName);
    people.Sort(lambda(a, b) => order.Compare(a, b));
    for var p in people do
      Console.WriteLine($"{p.City} {p.Age} {p.Name}");
    end;
  end Main;
end Program;
