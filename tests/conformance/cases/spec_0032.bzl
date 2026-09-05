enum Priority
  Low, Medium, High;

  public method IsUrgent(): bool => this = High;

  public property Name: string => (case this of
    Low => "Low",
    Medium => "Medium",
    High => "High");

  // FrozenArray<T> is stdlib §4.19 — not among §2.3's always-available names;
  //   naming it requires `import System.Collections.Generic;` (§2.8.3 rule 2).
  public static owned readonly All: FrozenArray<Priority> :=
      FrozenArray<Priority>.From([Low, Medium, High]);

  public static method Parse(text: string): Priority =>
      Priority.FromNameOrNull(text)
      ?? throw new ArgumentException($"unknown priority {text}", "text");
end Priority;
