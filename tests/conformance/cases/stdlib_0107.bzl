import System.Collections.Linq;
var evenSquares := (1..20).Where((x) => x mod 2 = 0).Select((x) => x * x);
var total := (1..100).Sum();
