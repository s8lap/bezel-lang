public closed class Shape ... end Shape;
public sealed class Circle: Shape ... end Circle;
public sealed class Square: Shape ... end Square;
public sealed class Triangle: Shape ... end Triangle;

var area := (case shape of
  var c: Circle => Math.Pi * c.Radius * c.Radius,
  var s: Square => s.Side * s.Side,
  var t: Triangle => 0.5 * t.Base * t.Height);                       // exhaustive: every leaf covered, no _ needed
