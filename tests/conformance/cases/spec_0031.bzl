enum Priority
  Low, Medium, High;

  public method IsUrgent(): bool => this = High;

  public property Name: string => (case this of
    Low => "Low",
    Medium => "Medium",
    High => "High");

  public static readonly All: Array<Priority> := [Low, Medium, High];

  public static method Parse(text: string): Priority?
    return (case text of
      "Low" => Low,
      "Medium" => Medium,
      "High" => High,
      _ => null);
  end Parse;
end Priority;
