module Query.Join;

record Dept(Id: int, Name: string);
record Employee(Name: string, DeptId: int);

public class Program
  public static method Main()
    var depts: Array<Dept> := [new Dept(1, "Eng"), new Dept(2, "Ops"), new Dept(3, "Legal")];
    var emps: Array<Employee> := [new Employee("ann", 1), new Employee("bob", 2), new Employee("cho", 1), new Employee("dee", 9)];
    var byId := depts.ToDictionary(lambda(d) => d.Id);
    for var e in emps do
      var name := byId.GetOrNull(e.DeptId)?.Name ?? "(none)";
      Console.WriteLine($"{e.Name} -> {name}");
    end;
    for var d in depts do
      Console.WriteLine($"{d.Name} {emps.Count(lambda(e) => e.DeptId = d.Id)}");
    end;
  end Main;
end Program;
