//! case: s26_5_0017
//! doc: spec
//! section: 26.5
//! title: specificity ordering picks the more specific extension across two imported modules
//! rule: Extensions on the most specific type are preferred (rule 3, §26.5.1).
//!   `ExtCircle`'s block on `Circle` is more specific than `ExtShape`'s block
//!   on `Shape`, and the ordering holds even though the two candidates come
//!   from two different imported modules, neither importing the other.
//! cites: 26.5, 26.5.1
//! expect: run
//! stdout: circle
module App;
import Shapes;
import ExtShape;
import ExtCircle;

public class Program
  public static method Main()
    using var c := new Circle();
    Console.WriteLine(c.Describe());
  end Main;
end Program;
