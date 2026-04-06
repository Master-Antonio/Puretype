using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace PuretypeUI.Services;

/// <summary>
/// Information about an available update from GitHub Releases.
/// </summary>
public sealed class UpdateInfo
{
    public required string TagName { get; init; }
    public required Version Version { get; init; }
    public required string DownloadUrl { get; init; }
    public required long AssetSize { get; init; }
    public required string ReleaseNotes { get; init; }
    public required string HtmlUrl { get; init; }

    /// <summary>
    /// Expected SHA-256 hash of the ZIP asset, parsed from the release body.
    /// Null when the release does not include a hash.
    /// </summary>
    public string? ExpectedSha256 { get; init; }
}

/// <summary>
/// Checks GitHub Releases for newer versions of PureType, downloads and installs updates.
/// Uses ETag caching to minimise GitHub API rate-limit consumption.
/// Verifies downloaded assets against a SHA-256 hash published in the release body.
/// </summary>
public sealed class GitHubUpdateService : IDisposable
{
    private const string GitHubOwner = "Master-Antonio";
    private const string GitHubRepo = "PureType";
    private const string ApiUrl = $"https://api.github.com/repos/{GitHubOwner}/{GitHubRepo}/releases?per_page=15";
    private const string UserAgent = $"{GitHubRepo}-Updater";

    // Regex to extract SHA-256 hash from the release body.
    // Expected format in the release notes:  SHA256: <64-char hex>
    private static readonly Regex Sha256Regex = new(
        @"SHA256:\s*([0-9a-fA-F]{64})",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    // Regex to strip pre-release suffixes (e.g. "-alpha", "-beta.2", "-rc1")
    // so that System.Version.TryParse can handle the numeric portion.
    private static readonly Regex PreReleaseSuffixRegex = new(
        @"-[a-zA-Z].*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    // Tray window constants matching injector.cpp
    private const string TrayWindowClass = "PureTypeTrayWindow";
    private const string TrayWindowTitle = "PureType";
    private const uint WM_PURETYPE_SHUTDOWN = 0x8003u; // WM_APP + 3

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    private readonly HttpClient _http;
    private bool _disposed;

    // ── ETag cache ──────────────────────────────────────────────────────────
    private static readonly string CacheDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PureType");
    private static readonly string ETagCachePath = Path.Combine(CacheDir, "update_cache.json");

    public GitHubUpdateService()
    {
        _http = new HttpClient();
        _http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue(UserAgent, AppVersion.Current));
        _http.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        _http.Timeout = TimeSpan.FromSeconds(30);
    }

    /// <summary>
    /// Checks the GitHub API for a newer release.
    /// Uses ETag / If-None-Match to avoid wasting API rate limit.
    /// Returns <see langword="null"/> if the current version is up to date or the check fails.
    /// </summary>
    public async Task<UpdateInfo?> CheckForUpdateAsync(CancellationToken ct = default)
    {
        try
        {
            // Load cached ETag + response body
            (string? cachedETag, string? cachedBody) = LoadETagCache();

            using var request = new HttpRequestMessage(HttpMethod.Get, ApiUrl);
            if (!string.IsNullOrEmpty(cachedETag))
                request.Headers.IfNoneMatch.Add(new EntityTagHeaderValue(cachedETag));

            using HttpResponseMessage response = await _http.SendAsync(request, ct).ConfigureAwait(false);

            string json;

            if (response.StatusCode == HttpStatusCode.NotModified && cachedBody is not null)
            {
                // API confirmed nothing changed — reuse cached body
                json = cachedBody;
            }
            else if (response.IsSuccessStatusCode)
            {
                json = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);

                // Persist new ETag + body for next call
                string? newETag = response.Headers.ETag?.Tag;
                SaveETagCache(newETag, json);
            }
            else
            {
                return null;
            }

            return ParseReleaseJson(json);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or JsonException)
        {
            return null;
        }
    }

    /// <summary>
    /// Downloads the release ZIP to a temp staging directory, verifies SHA-256 if available, and extracts it.
    /// Returns the path to the staging directory containing the extracted files.
    /// </summary>
    public async Task<string> DownloadAndExtractAsync(
        string downloadUrl,
        string? expectedSha256 = null,
        IProgress<double>? progress = null,
        CancellationToken ct = default)
    {
        string tempDir = Path.Combine(Path.GetTempPath(), "PureType_Update");

        // Clean up any previous staging
        if (Directory.Exists(tempDir))
            Directory.Delete(tempDir, recursive: true);

        Directory.CreateDirectory(tempDir);

        string zipPath = Path.Combine(tempDir, "update.zip");
        string extractDir = Path.Combine(tempDir, "staging");

        // Download with progress
        using (HttpResponseMessage response = await _http.GetAsync(downloadUrl, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false))
        {
            response.EnsureSuccessStatusCode();

            long totalBytes = response.Content.Headers.ContentLength ?? -1;
            long bytesRead = 0;

            await using FileStream fileStream = new(zipPath, FileMode.Create, FileAccess.Write, FileShare.None, bufferSize: 81920, useAsync: true);
            await using Stream contentStream = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);

            byte[] buffer = new byte[81920];
            int read;

            while ((read = await contentStream.ReadAsync(buffer, ct).ConfigureAwait(false)) > 0)
            {
                await fileStream.WriteAsync(buffer.AsMemory(0, read), ct).ConfigureAwait(false);
                bytesRead += read;

                if (totalBytes > 0)
                    progress?.Report((double)bytesRead / totalBytes * 100.0);
            }
        }

        // ── SHA-256 verification ────────────────────────────────────────────
        if (!string.IsNullOrWhiteSpace(expectedSha256))
        {
            string actualHash = await ComputeSha256Async(zipPath, ct).ConfigureAwait(false);
            if (!string.Equals(actualHash, expectedSha256, StringComparison.OrdinalIgnoreCase))
            {
                // Delete the corrupted / tampered file
                try { File.Delete(zipPath); } catch { /* best effort */ }
                throw new InvalidOperationException(
                    $"SHA-256 mismatch.\nExpected: {expectedSha256}\nActual:   {actualHash}");
            }
        }

        // Extract ZIP
        ZipFile.ExtractToDirectory(zipPath, extractDir, overwriteFiles: true);

        // If the ZIP contains a single root folder, use that as staging
        string[] topLevelDirs = Directory.GetDirectories(extractDir);
        string[] topLevelFiles = Directory.GetFiles(extractDir);

        if (topLevelDirs.Length == 1 && topLevelFiles.Length == 0)
            return topLevelDirs[0];

        return extractDir;
    }

    /// <summary>
    /// Performs the update by creating and executing a PowerShell updater script.
    /// This script waits for PureType processes to exit, copies files, and relaunches the app.
    /// </summary>
    public void InstallUpdate(string stagingDir)
    {
        string installDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
        string scriptPath = Path.Combine(Path.GetTempPath(), "PureType_Update", "puretype_updater.ps1");

        // Find tray PID
        uint trayPid = 0;
        IntPtr hTray = FindWindow(TrayWindowClass, TrayWindowTitle);
        if (hTray != IntPtr.Zero)
        {
            GetWindowThreadProcessId(hTray, out trayPid);
        }

        // Build PowerShell updater script
        var sb = new System.Text.StringBuilder(2048);
        sb.AppendLine("$ErrorActionPreference = 'SilentlyContinue'");
        sb.AppendLine($"$trayPid = {trayPid}");
        sb.AppendLine($"$stagingDir = '{EscapePsString(stagingDir)}'");
        sb.AppendLine($"$installDir = '{EscapePsString(installDir)}'");
        sb.AppendLine();
        sb.AppendLine("# Wait for PureType tray process to exit");
        sb.AppendLine("if ($trayPid -and $trayPid -ne 0) {");
        sb.AppendLine("    try {");
        sb.AppendLine("        $proc = Get-Process -Id $trayPid -ErrorAction SilentlyContinue");
        sb.AppendLine("        if ($proc) { $proc.WaitForExit(30000) | Out-Null }");
        sb.AppendLine("    } catch { }");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine("# Also wait for any remaining PuretypeUI or puretype processes");
        sb.AppendLine("$names = @('puretype', 'PuretypeUI')");
        sb.AppendLine("foreach ($n in $names) {");
        sb.AppendLine("    $procs = Get-Process -Name $n -ErrorAction SilentlyContinue");
        sb.AppendLine("    foreach ($p in $procs) {");
        sb.AppendLine("        try { $p.WaitForExit(10000) | Out-Null } catch { }");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine("# Brief delay for file handle release");
        sb.AppendLine("Start-Sleep -Seconds 2");
        sb.AppendLine();
        sb.AppendLine("# Copy all files from staging to install directory, preserving structure");
        sb.AppendLine("Get-ChildItem -Path $stagingDir -Recurse -Force | ForEach-Object {");
        sb.AppendLine(@"    $relativePath = $_.FullName.Substring($stagingDir.Length).TrimStart('\')");
        sb.AppendLine("    if (-not $_.PSIsContainer -and $relativePath -ieq 'puretype.ini') {");
        sb.AppendLine("        # Preserve user-local settings file during updates.");
        sb.AppendLine("        continue");
        sb.AppendLine("    }");
        sb.AppendLine("    $destPath = Join-Path $installDir $relativePath");
        sb.AppendLine("    if ($_.PSIsContainer) {");
        sb.AppendLine("        if (-not (Test-Path $destPath)) {");
        sb.AppendLine("            New-Item -ItemType Directory -Path $destPath -Force | Out-Null");
        sb.AppendLine("        }");
        sb.AppendLine("    } else {");
        sb.AppendLine("        $destDir = Split-Path $destPath -Parent");
        sb.AppendLine("        if (-not (Test-Path $destDir)) {");
        sb.AppendLine("            New-Item -ItemType Directory -Path $destDir -Force | Out-Null");
        sb.AppendLine("        }");
        sb.AppendLine("        try {");
        sb.AppendLine("            Copy-Item -Path $_.FullName -Destination $destPath -Force");
        sb.AppendLine("        } catch {");
        sb.AppendLine("            # Retry once after short delay (file lock)");
        sb.AppendLine("            Start-Sleep -Milliseconds 500");
        sb.AppendLine("            Copy-Item -Path $_.FullName -Destination $destPath -Force");
        sb.AppendLine("        }");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine("# Restart puretype.exe (the tray / injector)");
        sb.AppendLine("$exePath = Join-Path $installDir 'puretype.exe'");
        sb.AppendLine("if (Test-Path $exePath) {");
        sb.AppendLine("    Start-Process -FilePath $exePath");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine("# Clean up staging");
        sb.AppendLine("Start-Sleep -Seconds 2");
        sb.AppendLine("Remove-Item -Path (Split-Path $stagingDir -Parent) -Recurse -Force -ErrorAction SilentlyContinue");

        string script = sb.ToString();

        File.WriteAllText(scriptPath, script, System.Text.Encoding.UTF8);

        // Signal tray to shut down gracefully
        if (hTray != IntPtr.Zero)
        {
            PostMessage(hTray, WM_PURETYPE_SHUTDOWN, IntPtr.Zero, IntPtr.Zero);
        }

        // Launch the updater script
        Process.Start(new ProcessStartInfo
        {
            FileName = "powershell.exe",
            Arguments = $"-ExecutionPolicy Bypass -WindowStyle Hidden -File \"{scriptPath}\"",
            UseShellExecute = true,
            WindowStyle = ProcessWindowStyle.Hidden,
            CreateNoWindow = true
        });
    }

    /// <summary>
    /// Formats a byte count as a human-readable string (KB, MB, etc.).
    /// </summary>
    public static string FormatBytes(long bytes)
    {
        return bytes switch
        {
            < 1024 => $"{bytes} B",
            < 1024 * 1024 => $"{bytes / 1024.0:F1} KB",
            _ => $"{bytes / (1024.0 * 1024.0):F1} MB"
        };
    }

    private static string EscapePsString(string input)
        => input.Replace("'", "''");

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _http.Dispose();
    }

    // ── ETag cache ──────────────────────────────────────────────────────────
    private static (string? eTag, string? body) LoadETagCache()
    {
        try
        {
            if (!File.Exists(ETagCachePath))
                return (null, null);

            string json = File.ReadAllText(ETagCachePath);
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;

            string? eTag = root.GetProperty("etag").GetString();
            string? body = root.GetProperty("body").GetString();

            return (eTag, body);
        }
        catch
        {
            return (null, null);
        }
    }

    private static void SaveETagCache(string? eTag, string body)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ETagCachePath)!);

            using var writer = new StreamWriter(ETagCachePath, false, System.Text.Encoding.UTF8);
            using var jsonWriter = new Utf8JsonWriter(writer.BaseStream, new JsonWriterOptions { Indented = true });

            jsonWriter.WriteStartObject();
            jsonWriter.WriteString("etag", eTag);
            jsonWriter.WriteString("body", body);
            jsonWriter.WriteEndObject();
        }
        catch { /* best effort */ }
    }

    /// <summary>
    /// Strips pre-release suffixes ("-alpha", "-beta.2", "-rc1") so that
    /// System.Version.TryParse can handle the numeric portion.
    /// </summary>
    private static string StripPreReleaseSuffix(string version)
        => PreReleaseSuffixRegex.Replace(version, string.Empty);

    /// <summary>
    /// Parses the JSON response from /releases (an array of release objects).
    /// Iterates all releases (including pre-releases) and returns the newest
    /// one that is newer than the current local version and has a ZIP asset.
    /// </summary>
    private static UpdateInfo? ParseReleaseJson(string json)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement root = doc.RootElement;

        // The /releases endpoint returns an array; /releases/latest returns an object.
        // Support both for backward compatibility with cached responses.
        IEnumerable<JsonElement> releases = root.ValueKind == JsonValueKind.Array
            ? root.EnumerateArray()
            : new[] { root };

        string localVersionStr = StripPreReleaseSuffix(AppVersion.Current);
        if (!Version.TryParse(localVersionStr, out Version? localVersion))
            return null;

        foreach (JsonElement release in releases)
        {
            // Skip drafts
            if (release.TryGetProperty("draft", out JsonElement draftEl) && draftEl.GetBoolean())
                continue;

            string tagName = release.GetProperty("tag_name").GetString() ?? string.Empty;
            string versionString = StripPreReleaseSuffix(tagName.TrimStart('v', 'V'));

            if (!Version.TryParse(versionString, out Version? remoteVersion))
                continue;

            if (remoteVersion <= localVersion)
                continue;

            // Find the best ZIP asset
            string downloadUrl = string.Empty;
            long assetSize = 0;

            if (release.TryGetProperty("assets", out JsonElement assets))
            {
                foreach (JsonElement asset in assets.EnumerateArray())
                {
                    string name = asset.GetProperty("name").GetString() ?? string.Empty;
                    if (name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                    {
                        downloadUrl = asset.GetProperty("browser_download_url").GetString() ?? string.Empty;
                        assetSize = asset.GetProperty("size").GetInt64();
                        break;
                    }
                }
            }

            if (string.IsNullOrEmpty(downloadUrl))
                continue;

            string releaseNotes = release.TryGetProperty("body", out JsonElement bodyEl)
                ? bodyEl.GetString() ?? string.Empty
                : string.Empty;

            string htmlUrl = release.TryGetProperty("html_url", out JsonElement urlEl)
                ? urlEl.GetString() ?? string.Empty
                : string.Empty;

            string? expectedSha256 = null;

            // Parse SHA-256 hash from release notes
            Match sha256Match = Sha256Regex.Match(releaseNotes);
            if (sha256Match.Success)
            {
                expectedSha256 = sha256Match.Groups[1].Value;
            }

            return new UpdateInfo
            {
                TagName = tagName,
                Version = remoteVersion,
                DownloadUrl = downloadUrl,
                AssetSize = assetSize,
                ReleaseNotes = releaseNotes,
                HtmlUrl = htmlUrl,
                ExpectedSha256 = expectedSha256
            };
        }

        return null;
    }

    private static async Task<string> ComputeSha256Async(string filePath, CancellationToken ct)
    {
        using var sha256 = SHA256.Create();
        await using var stream = File.OpenRead(filePath);
        byte[] hash = await sha256.ComputeHashAsync(stream, ct).ConfigureAwait(false);
        return BitConverter.ToString(hash).Replace("-", "").ToLowerInvariant();
    }
}

