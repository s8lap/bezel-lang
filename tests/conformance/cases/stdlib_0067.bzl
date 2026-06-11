// Fast inner loop — direct dispatch
public method Sum(s: ReadOnlySpan<int>): int do ... end;

// Convenient and broad — virtual dispatch
public method Sum(s: IReadOnlySpan<int>): int do ... end;
