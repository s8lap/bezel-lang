using var pool: OwnedDictionary<string, Connection> := [
  "primary": Connect("p"),
  "backup":  Connect("b")
];
