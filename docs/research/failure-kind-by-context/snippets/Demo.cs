// C# (.NET 8). NOT COMPILED: no .NET toolchain on the research machine; written from memory.
// C#: no checked exceptions. The framework guidelines sort exception TYPES into usage errors
// (ArgumentException, InvalidOperationException: fix the caller) and execution errors, and
// ship a Try-pattern twin where failure is common. What is expected at a call is decided by
// which twin the caller picks and by which types its handler names.
using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Threading.Tasks;

static class Demo
{
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromMilliseconds(300) };

    // 1a. A file the user chose.
    static void OpenUserFile(string path)
    {
        try
        {
            Console.WriteLine($"user-file: {File.ReadAllBytes(path).Length} bytes");
        }
        catch (FileNotFoundException) { Console.WriteLine("user-file: no such file, pick another"); }
        catch (IOException e) { Console.WriteLine($"user-file: cannot read: {e.Message}"); }
        // UnauthorizedAccessException is not an IOException, and an empty path raises
        // ArgumentException (a "usage error" type) even when the user typed it.
    }

    // 1b. The database the service cannot run without: no handler. The unhandled exception
    // ends the process with a stack trace; Environment.FailFast(message, e) ends it at once.
    static byte[] OpenRequiredFile(string path) => File.ReadAllBytes(path);

    // 2a. Optional telemetry.   2b. The primary upstream: no handler; the request boundary has one.
    static async Task SendTelemetry()
    {
        try { await Http.GetAsync("http://127.0.0.1:1/metrics"); }
        catch (HttpRequestException) { Console.WriteLine("telemetry: dropped one sample"); }
        catch (TaskCanceledException) { } // HttpClient reports its own timeout as a cancellation
    }

    static Task<HttpResponseMessage> HandleRequest() => Http.GetAsync("http://127.0.0.1:1/api");

    // 3a. A number the user typed: the Try-Parse twin.   3b. A constant: the throwing twin.
    static void ParseUser(string s)
    {
        if (int.TryParse(s, out var n)) Console.WriteLine($"user-number: {n}");
        else Console.WriteLine("user-number: not a number, ask again");
    }

    static readonly int Port = int.Parse("8080"); // a FormatException here is a bug

    // 4a. A user's key: TryGetValue.   4b. The program's own key: the indexer.
    static readonly Dictionary<string, int> Limits = new() { ["retries"] = 3 };

    static void LookupUser(string k)
    {
        if (Limits.TryGetValue(k, out var v)) Console.WriteLine($"user-key: {v}");
        else Console.WriteLine("user-key: unknown setting");
    }

    static int LookupOwn(string k) => Limits[k]; // a KeyNotFoundException here is a bug
}
