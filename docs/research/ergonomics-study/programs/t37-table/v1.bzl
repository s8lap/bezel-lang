module Format.Table;

record Item(Name: string, Qty: int, Price: decimal);

public class Program
  public static method Main()
    var items: Array<Item> := [new Item("widget", 3, 2.50m), new Item("gadget", 10, 1.25m), new Item("gizmo", 1, 99.99m)];
    var grand := items.Sum(lambda(i) => i.Qty * i.Price);
    Console.WriteLine($"{"name",-10}{"qty",4}{"price",8}{"total",10}{"share",8}");
    Console.WriteLine(String.Repeat('-', 40));
    for var i in items do
      var total := i.Qty * i.Price;
      Console.WriteLine($"{i.Name,-10}{i.Qty,4}{i.Price,8:F2}{total,10:F2}{total / grand,8:P1}");
    end;
    Console.WriteLine(String.Repeat('-', 40));
    Console.WriteLine($"{"TOTAL",-10}{"",4}{"",8}{grand,10:F2}");
  end Main;
end Program;
