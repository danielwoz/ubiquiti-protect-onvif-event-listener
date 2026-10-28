# ONVIF Recorder — Windows installer

Native Windows GUI that installs / upgrades / uninstalls `onvif-recorder` on a
UniFi Dream Router or NVR over SSH. Builds to a single self-contained
`.exe`. Source of truth for the apt install commands it runs is
[`gh-pages/install.sh`](../gh-pages/install.sh).

## Build

Requires the .NET 8 SDK on Windows.

```powershell
dotnet restore OnvifRecorderInstaller.sln
dotnet build    OnvifRecorderInstaller.sln -c Release
dotnet test     OnvifRecorderInstaller.Tests
```

## Publish a single-file exe

```powershell
dotnet publish OnvifRecorderInstaller\OnvifRecorderInstaller.csproj `
  -c Release -r win-x64 --self-contained `
  -p:PublishSingleFile=true `
  -p:IncludeNativeLibrariesForSelfExtract=true `
  -p:PublishTrimmed=false `
  -o publish-out
```

CI publishes this exe automatically on `v*` tag pushes via
`.github/workflows/release-windows.yml`, passing `-p:Version=<tag version>`
so the exe knows its own version. Builds without it report 1.0.0, which
skips the `installer.min_version` check described below.

The installer project embeds [`gh-pages/channels.meta.json`](../gh-pages/channels.meta.json)
as its built-in channel list, so a change to that file also rebuilds the
installer in `windows-ci.yml`.

On Linux, from the repository root, the solution compiles (its tests cannot
run there, because of WPF) with:

```bash
docker run --rm -v "$PWD":/src -w /src mcr.microsoft.com/dotnet/sdk:8.0 \
  dotnet build windows-installer/OnvifRecorderInstaller.sln -p:EnableWindowsTargeting=true
```

## Layout

```
OnvifRecorderInstaller/
  App.xaml                       # Application-level resources + styles
  MainWindow.xaml                # Hosts a Frame; swaps Pages
  Pages/
    WelcomePage                  # Intro + overview
    EnableSshPage                # Guide user through enabling SSH in UniFi OS
    ConnectPage                  # Host/user/password form, test connection
    ChannelPage                  # Channel drop-down; reads Protect channel, pin, current channel
    InstallPage                  # Streams install/upgrade/uninstall over SSH
    DonePage                     # Shown after fresh install
    DashboardPage                # Shown on subsequent launches (creds saved)
  Services/
    ChannelCatalog               # channels.json fetch, validation and fallbacks
    Connection                   # In-memory credential holder
    CredentialStore              # Registry + DPAPI persistence
    InstallScripts               # Install/upgrade/uninstall shell scripts
    RegistryChannelCache         # Last valid channels.json in the registry
    SshService                   # SSH.NET wrapper
  Assets/                        # icon.ico, enable-ssh.png (optional)

OnvifRecorderInstaller.Tests/
  ChannelCatalogTests            # Manifest validation + fallback chain
  InstallScriptsTests            # Verify script generation
  CredentialStoreTests           # Verify DPAPI + registry round-trip
```

## Release channel

The channel page (shown for a first install and for **Re-install** on the
dashboard) offers the channels in a drop-down, each with the version
currently in its apt suite:

| Label | apt suite (channel id) |
|---|---|
| Latest (alpha) | `early-access` (alias `latest`) |
| Release Candidate | `rc` |
| Stable | `stable` |

"Latest" is only a label; the id written to the device stays
`early-access`. The dashboard shows the label too.

### Where the list comes from

The list is
[`channels.json`](https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/channels.json)
(schema 1; see [`gh-pages/README.md`](../gh-pages/README.md)). The installer
starts fetching it when it opens and uses the first of these that works:

1. this PC over HTTPS (5 s timeout, HTTP 200 only);
2. the router, with `curl` over the SSH connection (for a PC behind a proxy
   or without internet access);
3. the last list that passed validation, kept in
   `HKCU\Software\OnvifRecorderInstaller` (`ChannelsJson`,
   `ChannelsFetchedUtc`);
4. the built-in copy of `gh-pages/channels.meta.json` (labels, no
   versions).

The page says when the list did not come straight from this PC. Every
fetched list is validated before use or caching: schema must be `1`, the
body at most 64 KiB, each channel id must match
`^[a-z][a-z0-9-]{0,30}[a-z0-9]$` (the id goes into a root shell script; other
channels are dropped), duplicates keep the first, labels over 40 and
descriptions over 200 characters fall back to the id or nothing, and a
channel whose version is `null` or invalid is shown but cannot be picked.
Unknown fields are ignored. A newer schema, or an
`installer.min_version` above this installer's version, shows a link to
`installer.download_url`.

### Which channel is preselected

1. the channel pinned in `/etc/onvif-recorder/channel.pinned`;
2. else, when onvif-recorder is installed on the device, the channel it is
   on now (`/etc/onvif-recorder/channel`), so a re-install never moves it
   silently;
3. else the list's default (Latest);
4. else `early-access`.

A pin or current channel that the list does not name is added to the
drop-down, so it can still be kept.

### Pinning

The page also reads Protect's release channel from
`/data/unifi-core/config/runnables.yaml`.

- Choosing Protect's channel, unless it is the one already pinned, removes
  any pin, so the device keeps following Protect: `detect-channel.sh` (run
  by `postinst` and the daily `onvif-recorder-channel.timer`) moves it
  along when Protect's channel changes.
- Choosing a different channel, choosing any channel when Protect's channel
  or the existing pin could not be read, or keeping the channel already
  pinned writes `channel.pinned` exactly as the admin page's channel
  **Apply** does, so upgrades and the channel timer leave it alone. The
  admin page can change it later.

## Where credentials live

`HKEY_CURRENT_USER\Software\OnvifRecorderInstaller` — see
[`CredentialStore.cs`](OnvifRecorderInstaller/Services/CredentialStore.cs).
Secrets are DPAPI-encrypted with `CurrentUser` scope + a fixed entropy
string so a raw registry dump is not enough to recover them.

"Change router" / "Uninstall → forget credentials" deletes the whole
subkey, including the cached channel list.
