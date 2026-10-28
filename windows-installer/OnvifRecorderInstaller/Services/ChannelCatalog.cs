using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace OnvifRecorderInstaller.Services;

// Where the channel list shown on the channel page came from.
public enum ChannelSource {
    // channels.json fetched by this PC.
    Remote,
    // channels.json fetched by the router over SSH.
    Router,
    // The last channels.json that passed validation, from the registry.
    Cached,
    // channels.meta.json embedded in the installer: labels but no versions.
    BuiltIn,
}

public enum ManifestStatus {
    Ok,
    Invalid,
    // A schema newer than this installer reads.
    NewerSchema,
}

// One release channel as shown in the channel picker.
public sealed class ChannelOption {
    public ChannelOption(string id, string label, string description,
                         string? version, bool? published,
                         IReadOnlyList<string>? aliases = null) {
        Id = id;
        Label = label;
        Description = description;
        Version = version;
        Published = published;
        Aliases = aliases ?? Array.Empty<string>();
    }

    public string Id { get; }
    public string Label { get; }
    public string Description { get; }
    // The onvif-recorder version in the channel's apt suite; null when the
    // list does not say or the suite has no build.
    public string? Version { get; }
    // False when the list says the suite has no build; null when the list
    // carries no versions (the built-in list, or a channel the device uses
    // that the list does not name).
    public bool? Published { get; }
    public IReadOnlyList<string> Aliases { get; }

    public bool Selectable => Published != false;

    public string VersionText =>
        Version != null ? $" — {Version}"
        : Published == false ? " — no build published yet"
        : "";
}

// A validated channels.json (schema 1). The channel ids end up in a root
// shell script on the router, so every id passes
// InstallScripts.IsSafeChannelId; everything else is only shown in the UI.
public sealed class ChannelManifest {
    public const int SupportedSchema = 1;
    public const int MaxBytes = 64 * 1024;
    public const int MaxLabelLength = 40;
    public const int MaxDescriptionLength = 200;
    public const string DefaultDownloadUrl =
        "https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/releases/latest";

    private static readonly Regex VersionPattern = new(
        @"^[0-9][A-Za-z0-9.+~:-]{0,63}\z", RegexOptions.CultureInvariant);

    private ChannelManifest(IReadOnlyList<ChannelOption> channels, string defaultId,
                            string? minInstallerVersion, string downloadUrl) {
        Channels = channels;
        DefaultId = defaultId;
        MinInstallerVersion = minInstallerVersion;
        DownloadUrl = downloadUrl;
    }

    // In the published order.
    public IReadOnlyList<ChannelOption> Channels { get; }
    // Always a selectable channel's id.
    public string DefaultId { get; }
    // installer.min_version: installers older than this show a banner.
    public string? MinInstallerVersion { get; }
    // installer.download_url, or DefaultDownloadUrl; always https.
    public string DownloadUrl { get; }

    // The three suites with their ids as labels; used only when the
    // embedded list cannot be read.
    internal static ChannelManifest Minimal() => new(
        new[] {
            new ChannelOption("early-access", "early-access", "", null, null),
            new ChannelOption("rc", "rc", "", null, null),
            new ChannelOption("stable", "stable", "", null, null),
        },
        InstallScripts.DefaultChannelId, null, DefaultDownloadUrl);

    // Parses and validates a manifest. Unknown fields are ignored. Channels
    // with an unsafe or repeated id are dropped; an over-long or multi-line
    // label falls back to the id and such a description to empty; an
    // invalid version marks the channel as having no build. The manifest is
    // rejected when it is over MaxBytes, is not schema 1, or leaves no
    // selectable channel.
    public static ManifestStatus TryParse(string? json, out ChannelManifest? manifest) {
        manifest = null;
        if (string.IsNullOrWhiteSpace(json)) return ManifestStatus.Invalid;
        if (Encoding.UTF8.GetByteCount(json) > MaxBytes) return ManifestStatus.Invalid;

        JsonDocument doc;
        try {
            doc = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 16 });
        } catch (JsonException) {
            return ManifestStatus.Invalid;
        }
        using (doc) {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return ManifestStatus.Invalid;
            if (!root.TryGetProperty("schema", out var schema)
                || schema.ValueKind != JsonValueKind.Number
                || !schema.TryGetInt32(out var schemaNumber)) {
                return ManifestStatus.Invalid;
            }
            if (schemaNumber > SupportedSchema) return ManifestStatus.NewerSchema;
            if (schemaNumber != SupportedSchema) return ManifestStatus.Invalid;
            if (!root.TryGetProperty("channels", out var list)
                || list.ValueKind != JsonValueKind.Array) {
                return ManifestStatus.Invalid;
            }

            var entries = new List<JsonElement>();
            var ids = new HashSet<string>(StringComparer.Ordinal);
            foreach (var item in list.EnumerateArray()) {
                if (item.ValueKind != JsonValueKind.Object) continue;
                var id = StringProperty(item, "id");
                if (!InstallScripts.IsSafeChannelId(id) || !ids.Add(id!)) continue;
                entries.Add(item);
            }

            var names = new HashSet<string>(ids, StringComparer.Ordinal);
            var channels = new List<ChannelOption>();
            foreach (var item in entries) {
                channels.Add(ParseChannel(item, names));
            }

            var defaultId = PickDefault(channels, StringProperty(root, "default"));
            if (defaultId == null) return ManifestStatus.Invalid;

            string? minVersion = null;
            var downloadUrl = DefaultDownloadUrl;
            if (root.TryGetProperty("installer", out var installer)
                && installer.ValueKind == JsonValueKind.Object) {
                var min = StringProperty(installer, "min_version");
                if (min != null && VersionPattern.IsMatch(min)) minVersion = min;
                var url = StringProperty(installer, "download_url");
                if (IsHttpsUrl(url)) downloadUrl = url!;
            }

            manifest = new ChannelManifest(channels, defaultId, minVersion, downloadUrl);
            return ManifestStatus.Ok;
        }
    }

    // The channel with this id or alias, or null.
    public ChannelOption? Find(string? idOrAlias) {
        if (idOrAlias == null) return null;
        foreach (var c in Channels) {
            if (c.Id == idOrAlias) return c;
        }
        foreach (var c in Channels) {
            foreach (var a in c.Aliases) {
                if (a == idOrAlias) return c;
            }
        }
        return null;
    }

    // The channel id an alias stands for; any other value is returned as is.
    public string? Resolve(string? idOrAlias) => Find(idOrAlias)?.Id ?? idOrAlias;

    public string LabelFor(string? id) => Find(id)?.Label ?? id ?? "";

    // The channels plus an entry for each extra id the list does not name
    // (for example the device's pin), so it can still be shown and kept.
    // Ids that are not safe channel ids are left out.
    public IReadOnlyList<ChannelOption> WithExtraChannels(
            params (string? Id, string Note)[] extras) {
        var options = new List<ChannelOption>(Channels);
        foreach (var (id, note) in extras) {
            if (!InstallScripts.IsSafeChannelId(id) || Find(id) != null) continue;
            if (options.Exists(o => o.Id == id)) continue;
            options.Add(new ChannelOption(
                id!, $"{id} ({note})",
                "This channel is not in the published channel list.", null, null));
        }
        return options;
    }

    private static ChannelOption ParseChannel(JsonElement item, HashSet<string> names) {
        var id = StringProperty(item, "id")!;

        var aliases = new List<string>();
        if (item.TryGetProperty("aliases", out var list)
            && list.ValueKind == JsonValueKind.Array) {
            foreach (var a in list.EnumerateArray()) {
                var alias = a.ValueKind == JsonValueKind.String ? a.GetString() : null;
                if (InstallScripts.IsSafeChannelId(alias) && names.Add(alias!)) {
                    aliases.Add(alias!);
                }
            }
        }

        var label = CleanText(StringProperty(item, "label"), MaxLabelLength) ?? id;
        var description =
            CleanText(StringProperty(item, "description"), MaxDescriptionLength) ?? "";

        string? version = null;
        bool? published = null;
        if (item.TryGetProperty("version", out var v)) {
            var text = v.ValueKind == JsonValueKind.String ? v.GetString() : null;
            if (text != null && VersionPattern.IsMatch(text)) {
                version = text;
                published = true;
            } else {
                published = false;
            }
        }
        return new ChannelOption(id, label, description, version, published, aliases);
    }

    private static string? PickDefault(List<ChannelOption> channels, string? requested) {
        foreach (var c in channels) {
            if (!c.Selectable) continue;
            if (c.Id == requested) return c.Id;
            foreach (var a in c.Aliases) {
                if (a == requested) return c.Id;
            }
        }
        foreach (var c in channels) {
            if (c.Selectable) return c.Id;
        }
        return null;
    }

    private static string? StringProperty(JsonElement obj, string name) {
        return obj.TryGetProperty(name, out var p) && p.ValueKind == JsonValueKind.String
            ? p.GetString() : null;
    }

    // The trimmed text, or null when it is missing, empty, too long or
    // contains control characters.
    private static string? CleanText(string? text, int maxLength) {
        if (text == null) return null;
        foreach (var ch in text) {
            if (char.IsControl(ch)) return null;
        }
        text = text.Trim();
        return text.Length == 0 || text.Length > maxLength ? null : text;
    }

    private static bool IsHttpsUrl(string? url) {
        if (string.IsNullOrEmpty(url)) return false;
        foreach (var ch in url) {
            if (char.IsWhiteSpace(ch) || char.IsControl(ch)) return false;
        }
        return Uri.TryCreate(url, UriKind.Absolute, out var uri)
            && uri.Scheme == Uri.UriSchemeHttps
            && !string.IsNullOrEmpty(uri.Host);
    }
}

// The last channels.json that passed validation and when it was fetched.
public sealed record CachedManifest(string Json, DateTimeOffset? FetchedUtc);

public interface IChannelCache {
    CachedManifest? Read();
    void Write(string json, DateTimeOffset fetchedUtc);
}

public sealed class ChannelCatalogResult {
    public ChannelCatalogResult(ChannelManifest manifest, ChannelSource source,
                                DateTimeOffset? fetchedUtc, bool newerSchemaSeen,
                                bool installerOutdated) {
        Manifest = manifest;
        Source = source;
        FetchedUtc = fetchedUtc;
        NewerSchemaSeen = newerSchemaSeen;
        InstallerOutdated = installerOutdated;
    }

    public ChannelManifest Manifest { get; }
    public ChannelSource Source { get; }
    // When the cached list was fetched (Source == Cached).
    public DateTimeOffset? FetchedUtc { get; }
    // A fetched manifest used a schema this installer does not read.
    public bool NewerSchemaSeen { get; }
    // Show the "newer installer available" banner: a newer schema was
    // seen, or installer.min_version is above this installer's version.
    public bool InstallerOutdated { get; }
}

// Loads the release channel list: channels.json over HTTPS from this PC,
// else fetched by the router over SSH, else the last valid copy from the
// cache, else the list built into the installer. Every fetched manifest
// goes through ChannelManifest.TryParse, and only a valid one is cached.
public sealed class ChannelCatalog {
    public const string BuiltInResourceName = "channels.meta.json";
    public static readonly TimeSpan DefaultHttpTimeout = TimeSpan.FromSeconds(5);

    private readonly HttpMessageHandler _httpHandler;
    private readonly Func<CancellationToken, Task<string?>>? _routerFetch;
    private readonly IChannelCache? _cache;
    private readonly Version? _installerVersion;
    private readonly TimeSpan _httpTimeout;
    private readonly Func<DateTimeOffset> _clock;
    private readonly string _url;
    private Task<string?>? _remote;

    // routerFetch returns channels.json as the router fetched it, or null;
    // it may throw. builtInJson replaces the embedded channels.meta.json.
    public ChannelCatalog(HttpMessageHandler httpHandler,
                          Func<CancellationToken, Task<string?>>? routerFetch,
                          IChannelCache? cache,
                          Version? installerVersion = null,
                          TimeSpan? httpTimeout = null,
                          string? builtInJson = null,
                          Func<DateTimeOffset>? clock = null,
                          string url = InstallScripts.ChannelsUrl) {
        _httpHandler = httpHandler;
        _routerFetch = routerFetch;
        _cache = cache;
        _installerVersion = installerVersion;
        _httpTimeout = httpTimeout ?? DefaultHttpTimeout;
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _url = url;
        BuiltIn = ChannelManifest.TryParse(builtInJson ?? LoadBuiltInJson(), out var m)
                  == ManifestStatus.Ok
            ? m! : ChannelManifest.Minimal();
        Current = BuiltIn;
    }

    public ChannelManifest BuiltIn { get; }
    // The list from the last load, or the built-in list before any load.
    public ChannelManifest Current { get; private set; }

    // Starts the HTTPS fetch so it is usually done by the time the channel
    // page loads the list.
    public void StartRemoteFetch() {
        _remote ??= FetchRemoteAsync();
    }

    // Runs the fallback chain. Never throws. A successful HTTPS fetch is
    // reused by later loads; a failed one is retried by the next load.
    public async Task<ChannelCatalogResult> LoadAsync(CancellationToken ct = default) {
        var newer = false;

        _remote ??= FetchRemoteAsync();
        var remoteTask = _remote;
        var body = await remoteTask;
        var status = ChannelManifest.TryParse(body, out var manifest);
        if (status == ManifestStatus.Ok) {
            Remember(body!);
            return Finish(manifest!, ChannelSource.Remote, null, newer);
        }
        newer |= status == ManifestStatus.NewerSchema;
        if (_remote == remoteTask) _remote = null;

        if (_routerFetch != null) {
            string? routerBody;
            try {
                routerBody = await _routerFetch(ct);
            } catch (Exception) {
                routerBody = null;
            }
            status = ChannelManifest.TryParse(routerBody, out manifest);
            if (status == ManifestStatus.Ok) {
                Remember(routerBody!);
                return Finish(manifest!, ChannelSource.Router, null, newer);
            }
            newer |= status == ManifestStatus.NewerSchema;
        }

        CachedManifest? cached;
        try {
            cached = _cache?.Read();
        } catch (Exception) {
            cached = null;
        }
        if (cached != null
            && ChannelManifest.TryParse(cached.Json, out manifest) == ManifestStatus.Ok) {
            return Finish(manifest!, ChannelSource.Cached, cached.FetchedUtc, newer);
        }

        return Finish(BuiltIn, ChannelSource.BuiltIn, null, newer);
    }

    public static string? LoadBuiltInJson() {
        using var stream = typeof(ChannelCatalog).Assembly
            .GetManifestResourceStream(BuiltInResourceName);
        if (stream == null) return null;
        using var reader = new StreamReader(stream, Encoding.UTF8);
        return reader.ReadToEnd();
    }

    // This installer's version (1.0.0 for a build without -p:Version).
    public static Version? AssemblyVersion() =>
        typeof(ChannelCatalog).Assembly.GetName().Version;

    // True when minVersion names a version above the installer's. Builds
    // reporting 1.0.0 (local and CI builds without -p:Version) never are.
    public static bool IsInstallerOutdated(Version? installer, string? minVersion) {
        if (installer == null || minVersion == null) return false;
        var own = new Version(installer.Major, Math.Max(installer.Minor, 0),
                              Math.Max(installer.Build, 0));
        if (own == new Version(1, 0, 0)) return false;
        var min = NumericVersion(minVersion);
        return min != null && own < min;
    }

    // The leading major[.minor[.patch]] of a package version, or null.
    internal static Version? NumericVersion(string version) {
        var m = Regex.Match(
            version, @"^([0-9]{1,9})(?:\.([0-9]{1,9}))?(?:\.([0-9]{1,9}))?");
        if (!m.Success) return null;
        int Part(int i) => m.Groups[i].Success ? int.Parse(m.Groups[i].Value) : 0;
        return new Version(Part(1), Part(2), Part(3));
    }

    internal static string UserAgent(Version? version) {
        var v = version ?? new Version(1, 0, 0);
        return $"OnvifRecorderInstaller/{v.Major}.{Math.Max(v.Minor, 0)}.{Math.Max(v.Build, 0)}";
    }

    private ChannelCatalogResult Finish(ChannelManifest manifest, ChannelSource source,
                                        DateTimeOffset? fetchedUtc, bool newer) {
        Current = manifest;
        var outdated = newer
            || IsInstallerOutdated(_installerVersion, manifest.MinInstallerVersion);
        return new ChannelCatalogResult(manifest, source, fetchedUtc, newer, outdated);
    }

    private void Remember(string json) {
        try {
            _cache?.Write(json, _clock());
        } catch (Exception) {
            // A cache that cannot be written only loses the offline fallback.
        }
    }

    // channels.json as fetched over HTTPS, or null on any failure: a
    // non-200 status, a body over ChannelManifest.MaxBytes, invalid UTF-8,
    // or no complete answer within the timeout.
    private async Task<string?> FetchRemoteAsync() {
        try {
            using var cts = new CancellationTokenSource(_httpTimeout);
            using var client = new HttpClient(_httpHandler, disposeHandler: false) {
                Timeout = Timeout.InfiniteTimeSpan,
            };
            using var request = new HttpRequestMessage(HttpMethod.Get, _url);
            request.Headers.UserAgent.ParseAdd(UserAgent(_installerVersion));
            using var response = await client.SendAsync(
                request, HttpCompletionOption.ResponseHeadersRead, cts.Token)
                .ConfigureAwait(false);
            if (response.StatusCode != HttpStatusCode.OK) return null;
            if (response.Content.Headers.ContentLength > ChannelManifest.MaxBytes) {
                return null;
            }
            using var stream = await response.Content.ReadAsStreamAsync(cts.Token)
                .ConfigureAwait(false);
            var buffer = new MemoryStream();
            var chunk = new byte[8192];
            int read;
            while ((read = await stream.ReadAsync(chunk, cts.Token).ConfigureAwait(false)) > 0) {
                buffer.Write(chunk, 0, read);
                if (buffer.Length > ChannelManifest.MaxBytes) return null;
            }
            var text = new UTF8Encoding(false, true).GetString(
                buffer.GetBuffer(), 0, (int)buffer.Length);
            return text.TrimStart('﻿');
        } catch (Exception) {
            return null;
        }
    }
}
