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
      .GroupBy(lambda(o: Order): string => o.Customer)
      .Select(lambda(g: IGrouping<string, Order>): Summary => new Summary(g.Key, g.Count(), g.Aggregate(0m, lambda(acc: decimal, o: Order): decimal => acc + o.Amount)))
      .ToArray();
    // r40: Sum over a decimal selector inside a group returns 0 (compiler bug); Aggregate instead.
    // r40: OrderByDescending over a decimal key does not sort (compiler bug).
    summaries.Sort(lambda(x: Summary, y: Summary): int => y.Total.CompareTo(x.Total));
    for var s in summaries do
      Console.WriteLine($"{s.Customer} {s.Count} {s.Total:F2}");
    end;
  end Main;
end Program;
