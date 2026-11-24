// bug: B24
// title: Generic inference from an untyped lambda body picks the wrong result (silent)
// issue: #8394
// rests-on: spec §14.5 (v0.355) phase 2 infers `K` from the untyped lambda's body once `T` is fixed
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: cho
// stdout: cho
module P;
record Person(Name: string, Age: int);
public static class Seq
  public static method MaxBy<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end MaxBy;
end Seq;
public class Program
  public static method Main()
    var people: Array<Person> := [new Person("ann", 30), new Person("cho", 40), new Person("bob", 25)];
    Console.WriteLine(Seq.MaxBy(people, lambda(p) => p.Age).Name);              // expected cho
    Console.WriteLine(Seq.MaxBy<Person, int>(people, lambda(p) => p.Age).Name); // cho
  end Main;
end Program;
