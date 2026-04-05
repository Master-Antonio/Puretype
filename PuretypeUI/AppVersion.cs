namespace PuretypeUI
{
    /// <summary>
    /// Single source of truth for the application version.
    /// Updated automatically by the bump-version.ps1 script.
    /// </summary>
    internal static class AppVersion
    {
        public const string Current = "0.2.0";
        public const string DisplayName = "PureType";
        public const string FullDisplay = DisplayName + " v" + Current;
    }
}

