// General use — resizable
using var data := new Array<int>();
data.Add(1); data.Add(2); data.Add(3);

// Fixed-size discipline — pre-allocate via literal, don't resize
using var snapshot: Array<int> := [1, 2, 3];
// snapshot.Count == 3 throughout its lifetime; no Add calls

// Owning heap array
using var pool: OwnedArray<Connection> := [
  Connect("a"), Connect("b"), Connect("c")
];
