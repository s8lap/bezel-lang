var a: [3]int := [1, 2, 3];                       // inline array
using var b: Array<int> := [1, 2, 3];             // heap array
using var c: OwningArray<Connection> := [          // owning heap array
  Connect("a"), Connect("b"), Connect("c")
];

ProcessBuffer([1, 2, 3]);                          // type from parameter
