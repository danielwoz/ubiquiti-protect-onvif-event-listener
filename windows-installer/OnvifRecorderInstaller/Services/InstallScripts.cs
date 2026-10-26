using System;
using System.Collections.Generic;

namespace OnvifRecorderInstaller.Services;

public static class InstallScripts {
    public const string RepoUrl =
        "https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener";
    public const string Keyring =
        "/usr/share/keyrings/onvif-recorder-archive-keyring.gpg";
    public const string SourcesFile =
        "/etc/apt/sources.list.d/onvif-recorder.list";
    public const string ChannelFile = "/etc/onvif-recorder/channel";
    public const string PinnedChannelFile = "/etc/onvif-recorder/channel.pinned";
    public const string RunnablesYaml = "/data/unifi-core/config/runnables.yaml";

    public static readonly IReadOnlyList<string> ValidChannels =
        new[] { "stable", "rc", "early-access" };

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

    public static string BuildVersionScript() {
        return "dpkg-query -W -f='${Version}' onvif-recorder 2>/dev/null || echo ''";
    }

    // Returns the channel named in a script's output, or null when the
    // output is empty or not one of ValidChannels.
    public static string? ParseChannel(string? output) {
        var channel = (output ?? "").Trim();
        foreach (var v in ValidChannels) {
            if (v == channel) return v;
        }
        return null;
    }

    // The channel the channel page selects first: the one already pinned on
    // the device, then Protect's, then the one chosen in the last install.
    public static string DefaultChannel(
            string? pinnedChannel, string? protectChannel, string savedChannel) {
        return pinnedChannel ?? protectChannel ?? ParseChannel(savedChannel) ?? "stable";
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
        foreach (var v in ValidChannels) {
            if (v == channel) return;
        }
        throw new ArgumentException(
            $"invalid channel '{channel}' (expected stable|rc|early-access)");
    }
}
