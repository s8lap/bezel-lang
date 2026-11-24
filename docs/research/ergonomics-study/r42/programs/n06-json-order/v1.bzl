module Shop.Json;

import System.Serialization;

public record Customer(Name: string, Email: string): ISerializable, IDeserializable;
public record Line(Sku: string, Qty: int, Price: decimal): ISerializable, IDeserializable;

public class Order: ISerializable, IDeserializable
  public readonly property Id: int;
  public readonly property Customer: Customer;
  public owned readonly Lines: Array<Line>;

  public constructor(id: int, customer: Customer, owned lines: Array<Line>)
    Id := id;
    Customer := customer;
    Lines := lines;
  end constructor;

  public method Total(): decimal => Lines.Sum(lambda(l) => l.Qty * l.Price);
end Order;

public class Program
  public static method Main()
    var lines := new Array<Line>();
    lines.Add(new Line("A1", 2, 1.5m));
    lines.Add(new Line("B2", 1, 4m));
    using var order := new Order(7, new Customer("Ann", "ann@x.io"), move lines);

    using var s := new JsonSerializer();
    order.Serialize(s);
    var text := s.ToString();
    Console.WriteLine(text);

    using var d := new JsonDeserializer(text);
    using var back := Order.Deserialize(d);
    Console.WriteLine($"lines {back.Lines.Count} total {back.Total()}");
    var same := back.Id = order.Id and back.Customer = order.Customer and back.Lines.Count = order.Lines.Count;
    for var i in 0..<back.Lines.Count do
      same := same and back.Lines[i] = order.Lines[i];
    end;
    Console.WriteLine($"roundtrip equal: {same}");
  end Main;
end Program;
