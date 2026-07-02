// Sequence literals targeting different sequence types
using var list:  LinkedList<int> := [10, 20, 30];
using var stack: Stack<int>      := [1, 2, 3];        // 3 is on top
using var queue: Queue<int>      := [1, 2, 3];        // 1 is at the front
using var set:   Set<string>     := ["a", "b", "a"];  // collapses to {"a", "b"}

// Dictionary literal
using var ages: Dictionary<string, int> := [
  "alice": 30,
  "bob":   25,
  "carol": 41
];

// Nested: dictionary of arrays
using var groups: Dictionary<string, Array<int>> := [
  "primes": [2, 3, 5, 7],
  "evens":  [2, 4, 6, 8]
];

// Nested: dictionary of dictionaries
using var config: Dictionary<string, Dictionary<string, int>> := [
  "server": ["port": 8080, "timeout": 30],
  "client": ["retries": 3]
];

// Record-typed keys (no tuples in this version; use a named record)
record Point(X: int, Y: int);
using var labels: Dictionary<Point, string> := [
  Point(0, 0): "origin",
  Point(1, 0): "east"
];

// Owning collections — element expressions must produce owned values
using var conns: OwningArray<Connection> := [Connect("a"), Connect("b")];
using var pool:  OwningDictionary<string, Connection> := [
  "primary": Connect("p"),
  "backup":  Connect("b")
];
