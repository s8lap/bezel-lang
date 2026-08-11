// Non-null owned slots; initializer required at declaration
var fixed: [3 owned]Connection := [
  Connect("primary"), Connect("secondary"), Connect("tertiary")
];

// Nullable owned slots; defaults to all null
var pool: [16 owned]Connection?;
