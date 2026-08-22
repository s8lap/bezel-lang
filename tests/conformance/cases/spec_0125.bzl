const n := items.Count + 1;          // runtime const: int
const name := user.Name;             // runtime const: string (runtime-managed)
const conn := pool.Primary;          // runtime const: a borrow, source pool

const cfg := LoadConfig();           // error: an owned production; bind owned
                                     //   values with `using var` (§28.10)
