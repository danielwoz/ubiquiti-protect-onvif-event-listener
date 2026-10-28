using System;
using System.Text.RegularExpressions;

namespace OnvifRecorderInstaller.Services;

public static class InstallScripts {
    public const string RepoUrl =
        "https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener";
    // The release channel list (schema 1), regenerated with every apt
    // suite change; see ChannelCatalog.
    public const string ChannelsUrl = RepoUrl + "/channels.json";
    // The channel picked when nothing else chooses one: the early-access
    // suite, shown as "Latest (alpha)".
    public const string DefaultChannelId = "early-access";
    public const string Keyring =
        "/usr/share/keyrings/onvif-recorder-archive-keyring.gpg";
    public const string SourcesFile =
        "/etc/apt/sources.list.d/onvif-recorder.list";
    public const string ChannelFile = "/etc/onvif-recorder/channel";
    public const string PinnedChannelFile = "/etc/onvif-recorder/channel.pinned";
    public const string RunnablesYaml = "/data/unifi-core/config/runnables.yaml";

    // Channel ids are written into a root shell script and the apt source
    // line, so only lowercase letters, digits and '-' are accepted: 2 to 32
    // characters, starting with a letter and not ending with '-'. \z, not $,
    // so a trailing newline does not match.
    private static readonly Regex SafeChannelIdPattern = new(
        @"^[a-z][a-z0-9-]{0,30}[a-z0-9]\z", RegexOptions.CultureInvariant);

    public static bool IsSafeChannelId(string? id) =>
        id != null && SafeChannelIdPattern.IsMatch(id);

    // pinChannel writes channel.pinned with the same content as the admin
    // page's channel Apply, so detect-channel.sh (run by postinst and the
    // daily channel timer) keeps the channel instead of resetting it to
    // Protect's. Without it any earlier pin is removed and the device
    // follows Protect's channel.
    public static string BuildInstallScript(string channel, bool pinChannel) {
        ValidateChannel(channel);
        var pinCommand = pinChannel
            ? $"echo \"{channel}\" > {PinnedChannelFile}"
            : $"rm -f {PinnedChannelFile}";
        var pinMessage = pinChannel
            ? $"echo \"==> Channel: {channel} (pinned)\""
            : $"echo \"==> Channel: {channel} (follows the Protect release channel)\"";
        return $$"""
            set -e
            export DEBIAN_FRONTEND=noninteractive

            mkdir -p /usr/share/keyrings
            curl -fsSL {{RepoUrl}}/onvif-recorder.gpg -o /tmp/onvif-recorder.gpg
            gpg --dearmor < /tmp/onvif-recorder.gpg > {{Keyring}}
            chmod 0644 {{Keyring}}
            rm -f /tmp/onvif-recorder.gpg

            mkdir -p /etc/onvif-recorder
            echo "{{channel}}" > {{ChannelFile}}
            {{pinCommand}}
            {{pinMessage}}

            echo "deb [arch=arm64 signed-by={{Keyring}}] {{RepoUrl}} {{channel}} main" \
              > {{SourcesFile}}

            apt-get update \
              -o Dir::Etc::sourcelist={{SourcesFile}} \
              -o Dir::Etc::sourceparts=- \
              -o APT::Get::List-Cleanup=0

            apt-get install -y onvif-recorder

            dpkg-query -W -f='${Version}' onvif-recorder
            """;
    }

    public static string BuildUpgradeScript() {
        return $$"""
            set -e
            export DEBIAN_FRONTEND=noninteractive

            apt-get update \
              -o Dir::Etc::sourcelist={{SourcesFile}} \
              -o Dir::Etc::sourceparts=- \
              -o APT::Get::List-Cleanup=0

            apt-get install -y --only-upgrade onvif-recorder

            dpkg-query -W -f='${Version}' onvif-recorder
            """;
    }

    public static string BuildUninstallScript() {
        return $"""
            set -e
            export DEBIAN_FRONTEND=noninteractive

            apt-get purge -y onvif-recorder || true
            rm -f {SourcesFile}
            rm -f {Keyring}
            echo "done"
            """;
    }

    // Prints the Protect channel (stable | rc | early-access) or empty if
    // unknown. Mirrors debian/detect-channel.sh.
    public static string BuildDetectChannelScript() {
        return $$"""
            set -e
            RUNNABLES={{RunnablesYaml}}
            if [ ! -f "$RUNNABLES" ]; then
                echo ""
                exit 0
            fi
            RAW=$(awk '
                /^releaseChannels:/ { in_block=1; next }
                in_block && /^[^[:space:]]/ { in_block=0 }
                in_block && /^[[:space:]]+protect[[:space:]]*:/ {
                    sub(/^[^:]*:[[:space:]]*/, "")
                    gsub(/["'"'"']/, "")
                    sub(/[[:space:]]*#.*$/, "")
                    print; exit
                }
            ' "$RUNNABLES" 2>/dev/null | tr -d '[:space:]' | tr '[:upper:]' '[:lower:]')
            case "$RAW" in
                release)           echo "stable" ;;
                release-candidate) echo "rc" ;;
                beta|early-access) echo "early-access" ;;
                *)                 echo "" ;;
            esac
            """;
    }

    // Prints the channel pinned on the device by the admin page or an
    // earlier install, or nothing when it is not pinned.
    public static string BuildReadPinnedChannelScript() {
        return $"cat {PinnedChannelFile} 2>/dev/null | tr -d '[:space:]'";
    }

    // Prints "installed" or "not-installed" for the onvif-recorder package,
    // then the device's current channel (the channel file's content, which
    // can outlive a removed package).
    public static string BuildReadDeviceChannelScript() {
        return $$"""
            if dpkg-query -W -f='${Status}' onvif-recorder 2>/dev/null | grep -q ' installed$'; then
                echo installed
            else
                echo not-installed
            fi
            cat {{ChannelFile}} 2>/dev/null | tr -d '[:space:]'
            echo
            """;
    }

    // Prints channels.json as the router fetches it, cut off after one byte
    // more than ChannelManifest.MaxBytes so an oversize reply is rejected,
    // or nothing when curl fails.
    public static string BuildFetchManifestScript() {
        return $"curl -fsS -m 6 --max-filesize {ChannelManifest.MaxBytes} " +
               $"'{ChannelsUrl}' | head -c {ChannelManifest.MaxBytes + 1}";
    }

    public static string BuildVersionScript() {
        return "dpkg-query -W -f='${Version}' onvif-recorder 2>/dev/null || echo ''";
    }

    // Returns the channel named in a script's output, or null when the
    // output is empty or not a safe channel id. Ids missing from the
    // channel list are accepted, so a pin made by a newer installer or
    // admin page is kept.
    public static string? ParseChannel(string? output) {
        var channel = (output ?? "").Trim();
        return IsSafeChannelId(channel) ? channel : null;
    }

    // Reads BuildReadDeviceChannelScript's output.
    public static (bool Installed, string? Channel) ParseDeviceChannel(string? output) {
        var lines = (output ?? "").Replace("\r", "").Split('\n');
        var installed = lines[0].Trim() == "installed";
        var channel = lines.Length > 1 ? ParseChannel(lines[1]) : null;
        return (installed, channel);
    }

    // The channel the channel page selects first: the one pinned on the
    // device; else, when onvif-recorder is installed there, the channel it
    // is on now, so a re-install never moves it silently; else the channel
    // list's default; else Latest (early-access).
    public static string DefaultChannel(
            string? pinnedChannel, string? deviceChannel, bool packageInstalled,
            string? manifestDefault) {
        static string? Safe(string? id) => IsSafeChannelId(id) ? id : null;
        return Safe(pinnedChannel)
            ?? (packageInstalled ? Safe(deviceChannel) : null)
            ?? Safe(manifestDefault)
            ?? DefaultChannelId;
    }

    // An install pins the chosen channel when it differs from Protect's,
    // when Protect's channel is unknown (null; the channel page also passes
    // null when the device's pin could not be read, so an unseen pin is
    // never removed), or when the device is already pinned to it, so the
    // choice survives postinst and the daily channel timer. Choosing
    // Protect's channel otherwise leaves the device following Protect.
    public static bool ShouldPinChannel(
            string channel, string? protectChannel, string? pinnedChannel) {
        return protectChannel == null
            || channel != protectChannel
            || channel == pinnedChannel;
    }

    public static void ValidateChannel(string channel) {
        if (IsSafeChannelId(channel)) return;
        throw new ArgumentException(
            $"invalid channel '{channel}' (expected lowercase letters, digits and '-')");
    }
}
