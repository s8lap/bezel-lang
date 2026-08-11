public method Log(message: string, level: string := "INFO", timestamp: bool := true)
  // ...
end Log;

Log("System started", timestamp := false);
Log(level := "ERROR", message := "Failed");
