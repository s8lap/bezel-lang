// Probe f15 (proposal 0211 §3.4): an ordinary interface can be listed by types
// that can never be thrown. A record and a plain class both conform to the
// marker today; neither can reach a catch arm (spec §31.1: the throw operand is
// an `Exception`-derived class). The proposal restricts the marker to
// `Exception`-derived classes; this probe shows the status quo it tightens.
module F15;

public interface IFailure
end IFailure;

public record Point(X: int, Y: int): IFailure;

public class Plain: IFailure
end Plain;

public class Program
  public static method Main()
    var p := new Point(1, 2);
    var o: Object := p;
    Console.WriteLine($"a record conforms to the marker: {o is IFailure}");
    using var c := new Plain();
    var f: IFailure := c;
    Console.WriteLine($"a non-exception class conforms to the marker: {f.ToString()}");
  end Main;
end Program;
