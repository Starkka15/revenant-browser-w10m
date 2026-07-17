# Rebrands the CMake-generated appx manifest to "Revenant" (display name, tiles, splash, publisher)
# and injects the AppContainer network capabilities. CMake regenerates package.appxManifest on every
# configure/build WITHOUT these (it uses the target name "WebCoreRenderShell" + placeholder logos),
# so this runs between build passes (build-shell.bat) to apply them durably. Idempotent.
$m = 'Z:\w10m-webengine\build-wincairo\Source\WebCore\WebCoreRenderShell.dir\package.appxManifest'
if (-not (Test-Path $m)) { Write-Output 'MANIFEST_NOT_FOUND'; exit 0 }
$c = Get-Content -Raw $m

# --- Rebrand: name + publisher ---
$c = $c.Replace('<DisplayName>WebCoreRenderShell</DisplayName>', '<DisplayName>Revenant</DisplayName>')
$c = $c.Replace('DisplayName="WebCoreRenderShell"', 'DisplayName="Revenant"')
$c = $c.Replace('<PublisherDisplayName>CMake</PublisherDisplayName>', '<PublisherDisplayName>Revenant</PublisherDisplayName>')

# --- Rebrand: point tiles/splash/store logos at the bundled Revenant assets (deployed to Assets\) ---
$c = $c.Replace('WebCoreRenderShell.dir\StoreLogo.png', 'Assets\StoreLogo.png')
$c = $c.Replace('WebCoreRenderShell.dir\Logo.png', 'Assets\Square150x150Logo.png')
$c = $c.Replace('WebCoreRenderShell.dir\SmallLogo44x44.png', 'Assets\Square44x44Logo.png')
$c = $c.Replace('WebCoreRenderShell.dir\SplashScreen.png', 'Assets\SplashScreen.png')
$c = $c.Replace('BackgroundColor="#336699"', 'BackgroundColor="#026FD8"')

# --- rescap namespace (needed for the codeGeneration restricted capability below) ---
# codeGeneration = the UWP grant to allocate/execute dynamically generated code (JIT). Without it,
# VirtualProtectFromApp(PAGE_EXECUTE_*) is denied and the JIT's first call faults 0xC0000005 (DEP).
if ($c -notmatch 'restrictedcapabilities') {
  $c = $c -replace 'IgnorableNamespaces="([^"]*)"', 'IgnorableNamespaces="$1 rescap" xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"'
}

# --- AppContainer network capabilities + JIT codeGeneration (idempotent) ---
if ($c -notmatch 'internetClient') {
  $caps = "`t<Capabilities>`r`n`t`t<Capability Name=`"internetClient`" />`r`n`t`t<Capability Name=`"internetClientServer`" />`r`n`t`t<Capability Name=`"privateNetworkClientServer`" />`r`n`t`t<rescap:Capability Name=`"codeGeneration`" />`r`n`t</Capabilities>`r`n"
  $c = $c -replace '</Applications>', "</Applications>`r`n$caps"
} elseif ($c -notmatch 'codeGeneration') {
  $c = $c -replace '</Capabilities>', "`t<rescap:Capability Name=`"codeGeneration`" />`r`n`t</Capabilities>"
}

Set-Content -Path $m -Value $c -NoNewline
Write-Output 'MANIFEST_PATCHED_REVENANT'
