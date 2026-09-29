// Non-null owned slots; no default slots, so initialized explicitly (§7.9)
var fixed: [3 owned]Connection := [
  Connect("primary"), Connect("secondary"), Connect("tertiary")
];

// Nullable owned slots; defaults to all null
var pool: [16 owned]Connection?;
