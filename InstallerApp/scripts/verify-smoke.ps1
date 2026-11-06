param(
    [string]$Source,
    [string[]]$Destinations,
    [string]$Report,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

public sealed class SmokeTargetResult {
    public string Folder { get; set; }
    public long Files { get; set; }
    public long Bytes { get; set; }
    public double Seconds { get; set; }
}

public sealed class SmokeVerificationResult {
    public string Source { get; set; }
    public string Algorithm { get; set; } = "SHA-256; independent .NET implementation; outside benchmark timing";
    public long Files { get; set; }
    public long Directories { get; set; }
    public long Bytes { get; set; }
    public double SourceHashSeconds { get; set; }
    public List<SmokeTargetResult> Targets { get; set; } = new List<SmokeTargetResult>();
}

public static class SmokePayloadVerifier {
    sealed class Fingerprint {
        public long Length;
        public byte[] Hash;
    }

    static HashSet<string> Directories(string root) {
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string path in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories)) {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidOperationException("Reparse point in test output: " + path);
            paths.Add(Path.GetRelativePath(root, path));
        }
        return paths;
    }

    static byte[] Hash(string path, out long length) {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidOperationException("Reparse point in test output: " + path);
        using (var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
                                         1, FileOptions.SequentialScan))
        using (var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256)) {
            length = input.Length;
            byte[] buffer = ArrayPool<byte>.Shared.Rent((int)Math.Max(1, Math.Min(length, 1024 * 1024)));
            try {
                long remaining = length;
                while (remaining != 0) {
                    int read = input.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                    if (read == 0) throw new EndOfStreamException("Short test file: " + path);
                    sha.AppendData(buffer, 0, read);
                    remaining -= read;
                }
                return sha.GetHashAndReset();
            } finally { ArrayPool<byte>.Shared.Return(buffer); }
        }
    }

    public static SmokeVerificationResult Verify(string source, string[] targets) {
        source = Path.GetFullPath(source);
        var expectedDirectories = Directories(source);
        var expected = new Dictionary<string, Fingerprint>(StringComparer.OrdinalIgnoreCase);
        var result = new SmokeVerificationResult { Source = source };
        var timer = Stopwatch.StartNew();
        foreach (string path in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)) {
            long size;
            byte[] hash = Hash(path, out size);
            expected.Add(Path.GetRelativePath(source, path), new Fingerprint { Length = size, Hash = hash });
            result.Bytes += size;
            if (expected.Count % 20000 == 0)
                Console.WriteLine("Source payload hashed: " + expected.Count + " files");
        }
        result.Files = expected.Count;
        result.Directories = expectedDirectories.Count;
        result.SourceHashSeconds = timer.Elapsed.TotalSeconds;
        foreach (string folder in targets) {
            string target = Path.GetFullPath(folder);
            timer.Restart();
            if (!expectedDirectories.SetEquals(Directories(target)))
                throw new InvalidOperationException("Directory set mismatch: " + target);
            var actual = Directory.EnumerateFiles(target, "*", SearchOption.AllDirectories)
                .Select(path => Path.GetRelativePath(target, path)).ToArray();
            if (!new HashSet<string>(actual, StringComparer.OrdinalIgnoreCase).SetEquals(expected.Keys))
                throw new InvalidOperationException("File set mismatch: " + target);
            long checkedFiles = 0, checkedBytes = 0;
            Parallel.ForEach(actual, new ParallelOptions { MaxDegreeOfParallelism = 4 }, relative => {
                long size;
                byte[] hash = Hash(Path.Combine(target, relative), out size);
                Fingerprint wanted = expected[relative];
                if (size != wanted.Length || !hash.SequenceEqual(wanted.Hash))
                    throw new InvalidOperationException("Payload mismatch: " + Path.Combine(target, relative));
                Interlocked.Add(ref checkedBytes, size);
                if (Interlocked.Increment(ref checkedFiles) % 20000 == 0)
                    Console.WriteLine("Target payload checked: " + checkedFiles + " files; " + target);
            });
            result.Targets.Add(new SmokeTargetResult {
                Folder = target, Files = checkedFiles, Bytes = checkedBytes, Seconds = timer.Elapsed.TotalSeconds
            });
            Console.WriteLine("PASS SHA-256: " + target + "; files=" + checkedFiles + "; bytes=" + checkedBytes);
        }
        return result;
    }
}
'@

if ($SelfTest) {
    $root = Join-Path $env:TEMP ('mi-smoke-verifier-' + [guid]::NewGuid().ToString('N'))
    $sourceRoot = Join-Path $root 'source'
    $targetRoot = Join-Path $root 'target'
    foreach ($folder in @($sourceRoot, $targetRoot)) {
        [IO.Directory]::CreateDirectory((Join-Path $folder 'empty')) | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $folder 'payload.bin'), [byte[]](1, 2, 3, 4))
        $large = [byte[]]::new(2 * 1024 * 1024 + 17)
        $large[1048576] = 0xa5
        $large[-1] = 0x5a
        [IO.File]::WriteAllBytes((Join-Path $folder 'large.bin'), $large)
        [IO.File]::WriteAllBytes((Join-Path $folder 'empty.bin'), [byte[]]::new(0))
    }
    $null = [SmokePayloadVerifier]::Verify($sourceRoot, @($targetRoot))
    [IO.File]::WriteAllBytes((Join-Path $targetRoot 'payload.bin'), [byte[]](1, 2, 3, 5))
    $rejected = $false
    try { $null = [SmokePayloadVerifier]::Verify($sourceRoot, @($targetRoot)) }
    catch { $rejected = $_.Exception.ToString().Contains('Payload mismatch:') }
    if (-not $rejected) { throw 'Verifier accepted same-size corrupted output.' }
    [IO.File]::WriteAllBytes((Join-Path $targetRoot 'payload.bin'), [byte[]](1, 2, 3, 4))
    foreach ($offset in @(1048579, ($large.Length - 1))) {
        $stream = [IO.File]::Open((Join-Path $targetRoot 'large.bin'), [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
        try {
            $stream.Position = $offset
            $original = $stream.ReadByte()
            $stream.Position = $offset
            $stream.WriteByte($original -bxor 1)
        } finally { $stream.Dispose() }
        $rejected = $false
        try { $null = [SmokePayloadVerifier]::Verify($sourceRoot, @($targetRoot)) }
        catch { $rejected = $_.Exception.ToString().Contains('Payload mismatch:') }
        if (-not $rejected) { throw 'Verifier skipped a later buffer or short file tail.' }
        $stream = [IO.File]::Open((Join-Path $targetRoot 'large.bin'), [IO.FileMode]::Open, [IO.FileAccess]::Write)
        try { $stream.Position = $offset; $stream.WriteByte($original) } finally { $stream.Dispose() }
    }
    [IO.File]::WriteAllBytes((Join-Path $targetRoot 'extra.bin'), [byte[]](0))
    $rejected = $false
    try { $null = [SmokePayloadVerifier]::Verify($sourceRoot, @($targetRoot)) }
    catch { $rejected = $_.Exception.ToString().Contains('File set mismatch:') }
    if (-not $rejected) { throw 'Verifier accepted an extra file.' }
    $missingDirectoryTarget = Join-Path $root 'missing-empty-directory'
    [IO.Directory]::CreateDirectory($missingDirectoryTarget) | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $missingDirectoryTarget 'payload.bin'), [byte[]](1, 2, 3, 4))
    $rejected = $false
    try { $null = [SmokePayloadVerifier]::Verify($sourceRoot, @($missingDirectoryTarget)) }
    catch { $rejected = $_.Exception.ToString().Contains('Directory set mismatch:') }
    if (-not $rejected) { throw 'Verifier accepted a missing empty directory.' }
    Write-Output 'PASS verifier self-test: exact match, empty file, multi-buffer/tail corruption, extra file, missing empty directory.'
    return
}

if (-not $Source -or -not $Destinations -or -not $Report) {
    throw 'Provide Source, Destinations and a new Report path, or use -SelfTest.'
}
if (Test-Path -LiteralPath $Report) { throw 'Refusing to overwrite a verification report.' }
$result = [SmokePayloadVerifier]::Verify($Source, $Destinations)
$json = $result | ConvertTo-Json -Depth 5
$stream = [IO.File]::Open($Report, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $stream.Write($bytes, 0, $bytes.Length)
} finally { $stream.Dispose() }
Write-Output ('Verification report: ' + $Report)
