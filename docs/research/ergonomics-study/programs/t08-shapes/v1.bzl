module Model.Shapes;

public choice Shape
  Circle(radius: double),
  Rect(width: double, height: double),
  Triangle(a: double, b: double, c: double);

  public method Area(): double
    return (case this of
      Circle(var r) => Math.Pi * r * r,
      Rect(var w, var h) => w * h,
      Triangle(var a, var b, var c) => (var s := (a + b + c) / 2.0; Math.Sqrt(s * (s - a) * (s - b) * (s - c))));
  end Area;

  public method Name(): string => (case this of Circle(_) => "Circle", Rect(_, _) => "Rect", Triangle(_, _, _) => "Triangle");
end Shape;

public class Program
  public static method Main()
    var shapes: Array<Shape> := [Shape.Circle(1.0), Shape.Rect(2.0, 3.0), Shape.Triangle(3.0, 4.0, 5.0)];
    var total := 0.0;
    for var s in shapes do
      Console.WriteLine($"{s.Name()} {s.Area():F2}");
      total += s.Area();
    end;
    Console.WriteLine($"total {total:F2}");
    var largest := shapes.OrderByDescending(lambda(s) => s.Area()).First();
    Console.WriteLine($"largest {largest.Name()}");
  end Main;
end Program;
