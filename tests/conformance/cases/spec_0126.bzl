public class Pair
  public method Apply(x: int, y: long): string => "int,long";
  public method Apply(x: int, y: int): string  => "int,int";
end Pair;

var p := new Pair();
var a: int := 5;
var b: int := 9;

p.Apply(a, b);
// arg x (int): exact in both — tie.
// arg y (int): exact in Apply(int,int); requires int→long in Apply(int,long).
// Apply(int,int) is better-or-equal on x and strictly better on y ⇒ unambiguously better.
// Resolves to Apply(int, int).
