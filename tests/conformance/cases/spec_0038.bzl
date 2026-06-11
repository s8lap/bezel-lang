public sealed abstract class Shape
  public abstract method Area(): double;
end Shape;

public sealed class Circle(radius: double): Shape
  public readonly property Radius: double := radius;
  public override method Area(): double => Math.Pi * Radius * Radius;
end Circle;

public sealed class Square(side: double): Shape
  public readonly property Side: double := side;
  public override method Area(): double => Side * Side;
end Square;

public sealed class Triangle(baseLength: double, height: double): Shape
  public readonly property Base: double := baseLength;
  public readonly property Height: double := height;
  public override method Area(): double => 0.5 * Base * Height;
end Triangle;
