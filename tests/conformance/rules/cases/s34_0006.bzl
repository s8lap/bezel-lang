//! case: s34_0006
//! doc: spec
//! section: 34
//! title: additional recognized documentation tags are accepted
//! rule: Recognized tags include <remarks>, <example>, <see>, and <seealso>.
//! expect: accept
module Conf;

/// <summary>
/// Writes a message to the console.
/// </summary>
/// <remarks>
/// This is a multi-line remark explaining additional details
/// about the WriteMessage method.
/// </remarks>
/// <example>
/// <code>
/// WriteMessage("Hello, world!");
/// </code>
/// </example>
/// <see cref="System.Console.WriteLine"/>
/// <seealso cref="System.Console"/>
public class DocExample
  /// <summary>Writes the message.</summary>
  public static method WriteMessage(msg: string)
    Console.WriteLine(msg);
  end WriteMessage;
end DocExample;
