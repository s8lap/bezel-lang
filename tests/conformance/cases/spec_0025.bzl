enum Priority
  Low, Medium, High;

  public method IsUrgent(): bool => this = High;

  public property Name: string => case this
    Low => "Low",
    Medium => "Medium",
    High => "High"
  end;

  public static readonly All: Array<Priority> := [Low, Medium, High];

  public static method Parse(text: string): Priority?
    return case text
      "Low" => Low,
      "Medium" => Medium,
      "High" => High,
      _ => null
    end;
  end Parse;
end Priority;
