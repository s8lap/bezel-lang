// bug: NB5
// title: an array of choices does not deserialize past its first element
// issue: new
// rests-on: spec §9.8: a value-like serializable choice is admissible as a sequence element
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 2
module P;
import System.Serialization;
public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double);
end Shape;
public class Program
  public static method Main()
    using var d := new JsonDeserializer(@"[{""$case"":""Circle"",""r"":1},{""$case"":""Rect"",""w"":2,""h"":3}]");
    _ := d.BeginArray();
    var n := 0;
    while d.HasMoreElements() do
      var sh := Shape.Deserialize(d);
      n++;
    end;
    d.EndArray();
    Console.WriteLine(n);
  end Main;
end Program;
