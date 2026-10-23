# Shared by the native MSI (PowerShell 7) and MSIX (PowerShell 5.1) tests.

function New-DocumentTestProfile([string]$Path) {
    # Refuse stale history so a previous successful load cannot satisfy the oracle.
    if (Test-Path -LiteralPath $Path) { throw "Document test profile already exists: $Path" }
    New-Item -ItemType Directory -Path $Path | Out-Null
    @'
ui_language = "en"
auto_update = false
close_to_tray = false
restore_session = false
prompted_set_default_app = true
default_zoom = "fit-page"
'@ | Set-Content -LiteralPath (Join-Path $Path 'config.toml') -Encoding utf8
}

function Assert-DocumentLoad([string]$Profile, [string]$Document, [bool]$ExpectedLoaded = $true) {
    # Read a fresh profile after acceptance or normal exit. recordOpen persists
    # p and n only after acceptFreshLoad succeeds, unlike titles and exit codes.
    $history = Join-Path $Profile 'recent.json'
    $entries = @()
    if (Test-Path -LiteralPath $history) {
        $json = Get-Content -LiteralPath $history -Raw
        if (-not $json.TrimStart().StartsWith('[')) { throw "Recent history is not an array: $history" }
        # Malformed JSON must fail, including in the corrupt-document case.
        $decoded = $json | ConvertFrom-Json -ErrorAction Stop
        $entries = @($decoded)
    }
    $requested = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Document).ProviderPath).Replace('\', '/')
    $matching = @($entries | Where-Object {
        $_.p -is [string] -and $_.p.Replace('\', '/').Equals($requested, [StringComparison]::OrdinalIgnoreCase)
    })
    if (-not $ExpectedLoaded) {
        if ($entries.Count -ne 0) { throw "Rejected PDF created recent history in fresh profile: $history" }
        return
    }
    if ($matching.Count -ne 1 -or $entries.Count -ne 1) {
        throw "Expected one accepted document matching $requested in $history"
    }
    $pages = $matching[0].PSObject.Properties['n']
    if ($null -eq $pages -or $pages.Value -isnot [ValueType] -or
        $pages.Value -is [bool] -or $pages.Value -le 0 -or [math]::Truncate($pages.Value) -ne $pages.Value) {
        throw "Accepted document has no positive integer page count in $history"
    }
    Write-Host "Accepted document $requested with $($pages.Value) pages."
}

function Wait-DocumentLoad([Diagnostics.Process]$Process, [string]$Profile, [string]$Document) {
    # The title can precede acceptance. Do not close the window and cancel a load
    # while waiting for the first history write in this fresh profile.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt 60) {
        $Process.Refresh()
        if ($Process.HasExited) { throw 'Application exited before accepting the requested document.' }
        if (Test-Path -LiteralPath (Join-Path $Profile 'recent.json')) {
            Assert-DocumentLoad $Profile $Document
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Application did not record a successful document load in $Profile"
}
