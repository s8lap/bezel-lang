module Query.GroupBy;

record Order(Customer: string, Amount: decimal);
record Summary(Customer: string, Count: int, Total: decimal);

public class Program
  public static method Main()
    var orders: Array<Order> := [
      new Order("ann", 10.00m), new Order("bob", 5.50m), new Order("ann", 2.25m),
      new Order("cho", 30.00m), new Order("bob", 4.50m)
    ];
    var summaries := orders
      .GroupBy(lambda(o) => o.Customer)
      .Select(lambda(g) => new Summary(g.Key, g.Count(), g.Sum(lambda(o) => o.Amount)))
      .OrderByDescending(lambda(s) => s.Total)
      .ToArray();
    for var s in summaries do
      Console.WriteLine($"{s.Customer} {s.Count} {s.Total:F2}");
    end;
  end Main;
end Program;
