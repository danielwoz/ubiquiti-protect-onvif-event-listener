using System;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using OnvifRecorderInstaller.Services;
using Xunit;

namespace OnvifRecorderInstaller.Tests;

public class ChannelCatalogTests {
    private const string Download =
        "https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/releases/latest";

    private const string ValidChannels = """
        [
          { "id": "early-access", "aliases": ["latest"], "label": "Latest (alpha)",
            "description": "Every tagged release.", "version": "1.6.33",
            "updated_at": "2026-10-22T19:37:09Z" },
          { "id": "rc", "aliases": [], "label": "Release Candidate",
            "description": "Promoted from Latest.", "version": "1.6.32",
            "updated_at": "2026-10-23T12:51:07Z" },
          { "id": "stable", "aliases": [], "label": "Stable",
            "description": "Promoted from Release Candidate.", "version": "1.6.32",
            "updated_at": "2026-10-23T12:51:39Z" }
        ]
        """;

    private static readonly string Valid = Manifest(ValidChannels);

    // A schema 1 manifest with these channels; defaultJson and installerJson
    // are JSON values (or null to leave the field out).
    private static string Manifest(string channels,
                                   string? defaultJson = "\"early-access\"",
                                   string? installerJson = null,
                                   string schemaJson = "1",
                                   string extra = "") {
        var sb = new StringBuilder();
        sb.Append("{ \"schema\": ").Append(schemaJson);
        sb.Append(", \"generated_at\": \"2026-10-28T06:00:00Z\"");
        if (defaultJson != null) sb.Append(", \"default\": ").Append(defaultJson);
        sb.Append(", \"channels\": ").Append(channels);
        sb.Append(", \"installer\": ").Append(installerJson ??
            $"{{ \"min_version\": null, \"download_url\": \"{Download}\" }}");
        sb.Append(extra).Append(" }");
        return sb.ToString();
    }

    private static string Channel(string id, string? version = "1.0.0",
                                  string label = "Label") {
        var v = version == null ? "null" : $"\"{version}\"";
        return $"{{ \"id\": {Json(id)}, \"label\": {Json(label)}, \"version\": {v} }}";
    }

    private static string Json(string s) => System.Text.Json.JsonSerializer.Serialize(s);

    private static ChannelManifest Parse(string json) {
        Assert.Equal(ManifestStatus.Ok, ChannelManifest.TryParse(json, out var m));
        return m!;
    }

    // ---- Parsing and validation -------------------------------------------

    [Fact]
    public void TryParse_ValidSchema1_KeepsOrderAndFields() {
        var m = Parse(Valid);
        Assert.Equal(new[] { "early-access", "rc", "stable" },
                     m.Channels.Select(c => c.Id));
        Assert.Equal("early-access", m.DefaultId);
        var latest = m.Channels[0];
        Assert.Equal("Latest (alpha)", latest.Label);
        Assert.Equal("Every tagged release.", latest.Description);
        Assert.Equal("1.6.33", latest.Version);
        Assert.True(latest.Published);
        Assert.True(latest.Selectable);
        Assert.Equal(" — 1.6.33", latest.VersionText);
        Assert.Equal(new[] { "latest" }, latest.Aliases);
        Assert.Null(m.MinInstallerVersion);
        Assert.Equal(Download, m.DownloadUrl);
    }

    [Fact]
    public void TryParse_IgnoresUnknownFields() {
        var json = Manifest(
            "[{ \"id\": \"stable\", \"label\": \"Stable\", \"version\": \"1.6.32\", " +
            "\"notes\": { \"anything\": [1, 2, 3] }, \"weight\": 5 }]",
            "\"stable\"",
            $"{{ \"min_version\": null, \"download_url\": \"{Download}\", \"beta\": true }}",
            extra: ", \"future\": { \"x\": 1 }, \"mirrors\": [\"https://example.com\"]");
        var m = Parse(json);
        Assert.Single(m.Channels);
        Assert.Equal("stable", m.DefaultId);
    }

    [Fact]
    public void TryParse_NewerSchemaIsReported() {
        Assert.Equal(ManifestStatus.NewerSchema,
                     ChannelManifest.TryParse(Manifest(ValidChannels, schemaJson: "2"),
                                              out var m));
        Assert.Null(m);
    }

    [Theory]
    [InlineData("0")]
    [InlineData("\"1\"")]
    [InlineData("1.5")]
    [InlineData("null")]
    [InlineData("true")]
    public void TryParse_RejectsBadSchema(string schemaJson) {
        Assert.Equal(ManifestStatus.Invalid,
                     ChannelManifest.TryParse(Manifest(ValidChannels, schemaJson: schemaJson),
                                              out _));
    }

    [Fact]
    public void TryParse_RejectsMissingSchema() {
        var json = "{ \"default\": \"stable\", \"channels\": " +
                   "[{ \"id\": \"stable\", \"label\": \"Stable\", \"version\": \"1.0\" }] }";
        Assert.Equal(ManifestStatus.Invalid, ChannelManifest.TryParse(json, out _));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("not json")]
    [InlineData("[]")]
    [InlineData("{ \"schema\": 1 }")]
    [InlineData("{ \"schema\": 1, \"channels\": {} }")]
    [InlineData("{ \"schema\": 1, \"channels\": [] }")]
    [InlineData("{ \"schema\": 1, \"channels\": [")]
    public void TryParse_RejectsMalformedManifests(string? json) {
        Assert.Equal(ManifestStatus.Invalid, ChannelManifest.TryParse(json, out _));
    }

    [Theory]
    [InlineData("early-access; rm -rf /")]
    [InlineData("$(reboot)")]
    [InlineData("Stable")]
    [InlineData("")]
    [InlineData("abcdefghijklmnopqrstuvwxyzabcdefg")]
    [InlineData("-rc")]
    [InlineData("rc-")]
    [InlineData("rc\n")]
    [InlineData("r")]
    [InlineData("rc rc")]
    public void TryParse_DropsUnsafeIds(string badId) {
        var m = Parse(Manifest(
            $"[{Channel(badId)}, {Channel("stable")}]", Json(badId)));
        Assert.Equal(new[] { "stable" }, m.Channels.Select(c => c.Id));
        Assert.Equal("stable", m.DefaultId);
    }

    [Fact]
    public void TryParse_DropsNonStringIdsAndNonObjects() {
        var m = Parse(Manifest(
            $"[{{ \"id\": 5, \"label\": \"X\" }}, \"stable\", null, {Channel("rc")}]",
            "\"rc\""));
        Assert.Equal(new[] { "rc" }, m.Channels.Select(c => c.Id));
    }

    [Fact]
    public void TryParse_OnlyUnsafeIds_IsInvalid() {
        Assert.Equal(ManifestStatus.Invalid, ChannelManifest.TryParse(
            Manifest($"[{Channel("Stable")}, {Channel("rc;reboot")}]", "\"rc\""), out _));
    }

    [Fact]
    public void TryParse_KeepsTheFirstOfDuplicateIds() {
        var m = Parse(Manifest(
            $"[{Channel("rc", label: "First")}, {Channel("stable")}, " +
            $"{Channel("rc", label: "Second")}]", "\"rc\""));
        Assert.Equal(new[] { "rc", "stable" }, m.Channels.Select(c => c.Id));
        Assert.Equal("First", m.Channels[0].Label);
    }

    [Fact]
    public void TryParse_DropsAliasesThatAreIdsOrRepeatedOrUnsafe() {
        var m = Parse(Manifest(
            "[{ \"id\": \"early-access\", \"aliases\": [\"latest\", \"stable\", " +
            "\"Latest\", 7, \"latest\"], \"label\": \"L\", \"version\": \"1.0\" }, " +
            "{ \"id\": \"rc\", \"aliases\": [\"latest\", \"next\"], \"label\": \"R\", " +
            "\"version\": \"1.0\" }, " + Channel("stable") + "]"));
        Assert.Equal(new[] { "latest" }, m.Channels[0].Aliases);
        Assert.Equal(new[] { "next" }, m.Channels[1].Aliases);
        Assert.Equal("stable", m.Find("stable")!.Id);
    }

    [Fact]
    public void Find_AliasLatestResolvesToEarlyAccess() {
        var m = Parse(Valid);
        Assert.Equal("early-access", m.Find("latest")!.Id);
        Assert.Equal("early-access", m.Resolve("latest"));
        Assert.Equal("rc", m.Resolve("rc"));
        Assert.Equal("weekly", m.Resolve("weekly"));
        Assert.Null(m.Resolve(null));
        Assert.Equal("Latest (alpha)", m.LabelFor("latest"));
        Assert.Equal("weekly", m.LabelFor("weekly"));
    }

    [Theory]
    // Missing, unknown, unsafe and not-a-string defaults.
    [InlineData(null)]
    [InlineData("\"weekly\"")]
    [InlineData("\"rc; reboot\"")]
    [InlineData("3")]
    // A default without a published build.
    [InlineData("\"early-access\"")]
    public void TryParse_DefaultFallsBackToFirstPublishedChannel(string? defaultJson) {
        var m = Parse(Manifest(
            $"[{Channel("early-access", version: null)}, {Channel("rc")}, " +
            $"{Channel("stable")}]", defaultJson));
        Assert.Equal("rc", m.DefaultId);
    }

    [Fact]
    public void TryParse_DefaultMayBeAnAlias() {
        var m = Parse(Manifest(ValidChannels, "\"latest\""));
        Assert.Equal("early-access", m.DefaultId);
    }

    [Fact]
    public void TryParse_NoPublishedChannel_IsInvalid() {
        Assert.Equal(ManifestStatus.Invalid, ChannelManifest.TryParse(
            Manifest($"[{Channel("rc", version: null)}, {Channel("stable", version: null)}]",
                     "\"rc\""),
            out _));
    }

    [Theory]
    [InlineData("1234567890123456789012345678901234567890", true)]
    [InlineData("12345678901234567890123456789012345678901", false)]
    [InlineData("two\nlines", false)]
    [InlineData("tab\there", false)]
    [InlineData("   ", false)]
    [InlineData("", false)]
    public void TryParse_LabelLimits(string label, bool kept) {
        var m = Parse(Manifest($"[{Channel("stable", label: label)}]", "\"stable\""));
        Assert.Equal(kept ? label : "stable", m.Channels[0].Label);
    }

    [Fact]
    public void TryParse_MissingLabelFallsBackToId() {
        var m = Parse(Manifest("[{ \"id\": \"stable\", \"version\": \"1.0\" }]", "\"stable\""));
        Assert.Equal("stable", m.Channels[0].Label);
        Assert.Equal("", m.Channels[0].Description);
    }

    [Theory]
    [InlineData(200, true)]
    [InlineData(201, false)]
    public void TryParse_DescriptionLimit(int length, bool kept) {
        var description = new string('d', length);
        var m = Parse(Manifest(
            $"[{{ \"id\": \"stable\", \"label\": \"S\", \"version\": \"1.0\", " +
            $"\"description\": \"{description}\" }}]", "\"stable\""));
        Assert.Equal(kept ? description : "", m.Channels[0].Description);
    }

    [Fact]
    public void TryParse_DescriptionWithControlCharactersIsDropped() {
        var m = Parse(Manifest(
            "[{ \"id\": \"stable\", \"label\": \"S\", \"version\": \"1.0\", " +
            "\"description\": \"line\\nbreak\" }]", "\"stable\""));
        Assert.Equal("", m.Channels[0].Description);
    }

    [Theory]
    [InlineData("\"1.6.32; reboot\"")]
    [InlineData("\"v1.6.32\"")]
    [InlineData("\"\"")]
    [InlineData("\"1.6.32 \"")]
    [InlineData("1.6")]
    [InlineData("null")]
    [InlineData("\"1234567890123456789012345678901234567890123456789012345678901234x\"")]
    public void TryParse_BadOrNullVersionMeansNoBuild(string versionJson) {
        var m = Parse(Manifest(
            $"[{{ \"id\": \"rc\", \"label\": \"R\", \"version\": {versionJson} }}, " +
            $"{Channel("stable")}]", "\"stable\""));
        var rc = m.Channels[0];
        Assert.Null(rc.Version);
        Assert.False(rc.Published);
        Assert.False(rc.Selectable);
        Assert.Equal(" — no build published yet", rc.VersionText);
    }

    [Theory]
    [InlineData("1.6.32")]
    [InlineData("1.6.33-dev2")]
    [InlineData("1:1.6.32+b1~rc1")]
    public void TryParse_AcceptsDpkgVersions(string version) {
        var m = Parse(Manifest($"[{Channel("stable", version)}]", "\"stable\""));
        Assert.Equal(version, m.Channels[0].Version);
    }

    [Fact]
    public void TryParse_MissingVersionIsUnknownButSelectable() {
        var m = Parse(Manifest("[{ \"id\": \"stable\", \"label\": \"S\" }]", "\"stable\""));
        Assert.Null(m.Channels[0].Version);
        Assert.Null(m.Channels[0].Published);
        Assert.True(m.Channels[0].Selectable);
        Assert.Equal("", m.Channels[0].VersionText);
    }

    [Fact]
    public void TryParse_RejectsOversizeBody() {
        var json = Valid.Insert(1, new string(' ', ChannelManifest.MaxBytes));
        Assert.Equal(ManifestStatus.Invalid, ChannelManifest.TryParse(json, out _));
        var atLimit = Valid.Insert(
            1, new string(' ', ChannelManifest.MaxBytes - Encoding.UTF8.GetByteCount(Valid)));
        Assert.Equal(ManifestStatus.Ok, ChannelManifest.TryParse(atLimit, out _));
    }

    [Theory]
    [InlineData("{ \"min_version\": \"1.6.40\", \"download_url\": \"https://example.com/get\" }",
                "1.6.40", "https://example.com/get")]
    [InlineData("{ \"min_version\": \"latest\", \"download_url\": \"http://example.com/\" }",
                null, Download)]
    [InlineData("{ \"min_version\": 5, \"download_url\": \"javascript:alert(1)\" }",
                null, Download)]
    [InlineData("{ \"download_url\": \"https://example.com/a b\" }", null, Download)]
    [InlineData("{ \"download_url\": \"file:///C:/Windows/notepad.exe\" }", null, Download)]
    [InlineData("\"none\"", null, Download)]
    public void TryParse_InstallerBlock(string installerJson, string? minVersion, string url) {
        var m = Parse(Manifest(ValidChannels, installerJson: installerJson));
        Assert.Equal(minVersion, m.MinInstallerVersion);
        Assert.Equal(url, m.DownloadUrl);
    }

    [Fact]
    public void WithExtraChannels_AddsUnknownSafeIdsOnce() {
        var m = Parse(Valid);
        var options = m.WithExtraChannels(
            ("weekly", "current pin"), ("weekly", "current channel"),
            ("rc", "current channel"), ("latest", "current pin"),
            ("Bad;id", "current pin"), (null, "current channel"));
        Assert.Equal(new[] { "early-access", "rc", "stable", "weekly" },
                     options.Select(o => o.Id));
        var weekly = options[3];
        Assert.Equal("weekly (current pin)", weekly.Label);
        Assert.True(weekly.Selectable);
        Assert.Null(weekly.Version);
    }

    // ---- Built-in list ---------------------------------------------------

    [Fact]
    public void BuiltIn_EmbeddedMetaParses() {
        var json = ChannelCatalog.LoadBuiltInJson();
        Assert.NotNull(json);
        var m = Parse(json!);
        Assert.Equal(new[] { "early-access", "rc", "stable" },
                     m.Channels.Select(c => c.Id));
        Assert.Equal("early-access", m.DefaultId);
        Assert.Equal("Latest (alpha)", m.Channels[0].Label);
        Assert.Equal("early-access", m.Resolve("latest"));
        Assert.All(m.Channels, c => {
            Assert.Null(c.Version);
            Assert.True(c.Selectable);
            Assert.NotEqual(c.Id, c.Label);
        });
    }

    [Fact]
    public void Catalog_UnreadableBuiltIn_FallsBackToTheThreeSuites() {
        var catalog = new ChannelCatalog(
            FakeHandler.Status(HttpStatusCode.NotFound), null, null,
            builtInJson: "not json");
        Assert.Equal(new[] { "early-access", "rc", "stable" },
                     catalog.BuiltIn.Channels.Select(c => c.Id));
        Assert.Equal("early-access", catalog.BuiltIn.DefaultId);
    }

    // ---- Fallback chain --------------------------------------------------

    [Fact]
    public async Task Load_Remote200_UsesRemoteAndCachesIt() {
        var handler = FakeHandler.Ok(Valid);
        var cache = new MemoryCache();
        var when = new DateTimeOffset(2026, 10, 28, 6, 0, 0, TimeSpan.Zero);
        var router = new FakeRouter("unused");
        var catalog = new ChannelCatalog(handler, router.Fetch, cache,
                                         new Version(1, 6, 34), clock: () => when);
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.Remote, result.Source);
        Assert.Equal("1.6.33", result.Manifest.Channels[0].Version);
        Assert.Same(result.Manifest, catalog.Current);
        Assert.Equal(1, cache.Writes);
        Assert.Equal(Valid, cache.Json);
        Assert.Equal(when, cache.FetchedUtc);
        Assert.Equal(0, router.Calls);
        Assert.False(result.InstallerOutdated);
        Assert.Equal(InstallScripts.ChannelsUrl, handler.LastUri);
        Assert.Equal("OnvifRecorderInstaller/1.6.34", handler.LastUserAgent);
    }

    [Theory]
    [InlineData(HttpStatusCode.NotFound)]
    [InlineData(HttpStatusCode.InternalServerError)]
    [InlineData(HttpStatusCode.MovedPermanently)]
    [InlineData(HttpStatusCode.NoContent)]
    public async Task Load_RemoteNot200_UsesRouter(HttpStatusCode status) {
        var cache = new MemoryCache();
        var catalog = new ChannelCatalog(FakeHandler.Status(status),
                                         new FakeRouter(Valid).Fetch, cache);
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.Router, result.Source);
        Assert.Equal(1, cache.Writes);
        Assert.Equal(Valid, cache.Json);
    }

    [Fact]
    public async Task Load_RemoteTimeout_UsesRouter() {
        var handler = new FakeHandler(async (_, ct) => {
            await Task.Delay(TimeSpan.FromSeconds(30), ct);
            return new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(Valid) };
        });
        var catalog = new ChannelCatalog(handler, new FakeRouter(Valid).Fetch, new MemoryCache(),
                                         httpTimeout: TimeSpan.FromMilliseconds(50));
        var started = DateTime.UtcNow;
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.Router, result.Source);
        Assert.True(DateTime.UtcNow - started < TimeSpan.FromSeconds(10));
    }

    [Fact]
    public async Task Load_RemoteNetworkError_UsesRouter() {
        var handler = new FakeHandler((_, _) =>
            throw new HttpRequestException("no route to host"));
        var catalog = new ChannelCatalog(handler, new FakeRouter(Valid).Fetch, new MemoryCache());
        Assert.Equal(ChannelSource.Router, (await catalog.LoadAsync()).Source);
    }

    [Fact]
    public async Task Load_RemoteOversize_UsesRouter() {
        var big = Valid.Insert(1, new string(' ', ChannelManifest.MaxBytes));
        var catalog = new ChannelCatalog(FakeHandler.Ok(big), new FakeRouter(Valid).Fetch,
                                         new MemoryCache());
        Assert.Equal(ChannelSource.Router, (await catalog.LoadAsync()).Source);
    }

    [Fact]
    public async Task Load_RemoteInvalid_UsesRouterAndDoesNotCacheIt() {
        var cache = new MemoryCache();
        var catalog = new ChannelCatalog(FakeHandler.Ok("{ \"schema\": 1 }"),
                                         new FakeRouter(Valid).Fetch, cache);
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.Router, result.Source);
        Assert.Equal(Valid, cache.Json);
    }

    [Fact]
    public async Task Load_RemoteAndRouterFail_UsesCache() {
        var fetched = new DateTimeOffset(2026, 10, 20, 8, 30, 0, TimeSpan.Zero);
        var cache = new MemoryCache { Json = Valid, FetchedUtc = fetched };
        var catalog = new ChannelCatalog(FakeHandler.Status(HttpStatusCode.NotFound),
                                         new FakeRouter(null).Fetch, cache);
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.Cached, result.Source);
        Assert.Equal(fetched, result.FetchedUtc);
        Assert.Equal("1.6.33", result.Manifest.Channels[0].Version);
        Assert.Equal(0, cache.Writes);
    }

    [Fact]
    public async Task Load_RouterThrows_UsesCache() {
        var cache = new MemoryCache { Json = Valid };
        var catalog = new ChannelCatalog(
            FakeHandler.Status(HttpStatusCode.NotFound),
            _ => throw new InvalidOperationException("ssh failed"), cache);
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.Cached, result.Source);
        Assert.Null(result.FetchedUtc);
    }

    [Fact]
    public async Task Load_RouterAsyncFailure_UsesCache() {
        var cache = new MemoryCache { Json = Valid };
        var catalog = new ChannelCatalog(
            FakeHandler.Status(HttpStatusCode.NotFound),
            async _ => {
                await Task.Yield();
                throw new TimeoutException("ssh timed out");
            },
            cache);
        Assert.Equal(ChannelSource.Cached, (await catalog.LoadAsync()).Source);
    }

    [Fact]
    public async Task Load_NoRouterFetcher_UsesCache() {
        var cache = new MemoryCache { Json = Valid };
        var catalog = new ChannelCatalog(FakeHandler.Status(HttpStatusCode.NotFound),
                                         null, cache);
        Assert.Equal(ChannelSource.Cached, (await catalog.LoadAsync()).Source);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("{ \"schema\": 1, \"channels\": [] }")]
    [InlineData("garbage")]
    public async Task Load_CacheEmptyOrInvalid_UsesBuiltIn(string? cached) {
        var cache = new MemoryCache { Json = cached };
        var catalog = new ChannelCatalog(FakeHandler.Status(HttpStatusCode.NotFound),
                                         new FakeRouter("").Fetch, cache);
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.BuiltIn, result.Source);
        Assert.Same(catalog.BuiltIn, result.Manifest);
        Assert.Equal(0, cache.Writes);
        Assert.False(result.InstallerOutdated);
    }

    [Fact]
    public async Task Load_CacheThatThrows_UsesBuiltIn() {
        var catalog = new ChannelCatalog(FakeHandler.Status(HttpStatusCode.NotFound),
                                         null, new ThrowingCache());
        Assert.Equal(ChannelSource.BuiltIn, (await catalog.LoadAsync()).Source);
    }

    [Fact]
    public async Task Load_CacheWriteFailure_StillUsesRemote() {
        var catalog = new ChannelCatalog(FakeHandler.Ok(Valid), null, new ThrowingCache());
        Assert.Equal(ChannelSource.Remote, (await catalog.LoadAsync()).Source);
    }

    [Fact]
    public async Task Load_NewerSchema_IsNotCachedAndRaisesTheBanner() {
        var newer = Manifest(ValidChannels, schemaJson: "2");
        var cache = new MemoryCache();
        var catalog = new ChannelCatalog(FakeHandler.Ok(newer), new FakeRouter(newer).Fetch,
                                         cache, new Version(1, 6, 34));
        var result = await catalog.LoadAsync();
        Assert.Equal(ChannelSource.BuiltIn, result.Source);
        Assert.True(result.NewerSchemaSeen);
        Assert.True(result.InstallerOutdated);
        Assert.Equal(0, cache.Writes);
        Assert.Equal(Download, result.Manifest.DownloadUrl);
    }

    [Fact]
    public async Task Load_ReusesASuccessfulRemoteFetch() {
        var handler = FakeHandler.Ok(Valid);
        var catalog = new ChannelCatalog(handler, null, new MemoryCache());
        catalog.StartRemoteFetch();
        await catalog.LoadAsync();
        await catalog.LoadAsync();
        Assert.Equal(1, handler.Calls);
    }

    [Fact]
    public async Task Load_RetriesTheRemoteFetchAfterAFailure() {
        var calls = 0;
        var handler = new FakeHandler((_, _) => {
            calls++;
            var status = calls == 1 ? HttpStatusCode.ServiceUnavailable : HttpStatusCode.OK;
            return Task.FromResult(new HttpResponseMessage(status) {
                Content = new StringContent(Valid),
            });
        });
        var catalog = new ChannelCatalog(handler, null, new MemoryCache());
        Assert.Equal(ChannelSource.BuiltIn, (await catalog.LoadAsync()).Source);
        Assert.Equal(ChannelSource.Remote, (await catalog.LoadAsync()).Source);
        Assert.Equal(2, calls);
    }

    [Theory]
    [InlineData("1.6.40", 1, 6, 34, true)]
    [InlineData("1.6.34", 1, 6, 34, false)]
    [InlineData("1.6.30", 1, 6, 34, false)]
    // Builds without -p:Version report 1.0.0 and never show the banner.
    [InlineData("9.9.9", 1, 0, 0, false)]
    public async Task Load_MinVersionAboveTheInstallerRaisesTheBanner(
            string minVersion, int major, int minor, int build, bool expected) {
        var json = Manifest(ValidChannels, installerJson:
            $"{{ \"min_version\": \"{minVersion}\", \"download_url\": \"{Download}\" }}");
        var catalog = new ChannelCatalog(FakeHandler.Ok(json), null, new MemoryCache(),
                                         new Version(major, minor, build));
        var result = await catalog.LoadAsync();
        Assert.Equal(expected, result.InstallerOutdated);
        Assert.False(result.NewerSchemaSeen);
    }

    [Theory]
    [InlineData(1, 6, 32, 0, "1.6.34", true)]
    [InlineData(1, 6, 34, 0, "1.6.34", false)]
    [InlineData(1, 6, 35, 0, "1.6.34", false)]
    [InlineData(1, 6, 32, 0, "1.6.33-dev2", true)]
    [InlineData(1, 6, 32, 0, "1.7", true)]
    [InlineData(1, 6, 32, 0, "2", true)]
    [InlineData(1, 0, 0, 0, "9.9.9", false)]
    [InlineData(1, 6, 32, 0, null, false)]
    [InlineData(1, 6, 32, 0, "latest", false)]
    public void IsInstallerOutdated_ComparesNumericVersions(
            int major, int minor, int build, int revision, string? min, bool expected) {
        Assert.Equal(expected, ChannelCatalog.IsInstallerOutdated(
            new Version(major, minor, build, revision), min));
    }

    [Fact]
    public void IsInstallerOutdated_UnknownInstallerVersion_IsFalse() {
        Assert.False(ChannelCatalog.IsInstallerOutdated(null, "9.9.9"));
    }

    // ---- Fakes -----------------------------------------------------------

    private sealed class FakeHandler : HttpMessageHandler {
        private readonly Func<HttpRequestMessage, CancellationToken,
                              Task<HttpResponseMessage>> _send;

        public FakeHandler(
                Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> send) {
            _send = send;
        }

        public int Calls { get; private set; }
        public string? LastUri { get; private set; }
        public string? LastUserAgent { get; private set; }

        public static FakeHandler Ok(string body) => new((_, _) =>
            Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) {
                Content = new StringContent(body),
            }));

        public static FakeHandler Status(HttpStatusCode status) => new((_, _) =>
            Task.FromResult(new HttpResponseMessage(status) {
                Content = new StringContent(Valid),
            }));

        protected override Task<HttpResponseMessage> SendAsync(
                HttpRequestMessage request, CancellationToken ct) {
            Calls++;
            LastUri = request.RequestUri?.ToString();
            LastUserAgent = request.Headers.UserAgent.ToString();
            return _send(request, ct);
        }
    }

    private sealed class FakeRouter {
        private readonly string? _body;

        public FakeRouter(string? body) {
            _body = body;
        }

        public int Calls { get; private set; }

        public Task<string?> Fetch(CancellationToken ct) {
            Calls++;
            return Task.FromResult(_body);
        }
    }

    private sealed class MemoryCache : IChannelCache {
        public string? Json { get; set; }
        public DateTimeOffset? FetchedUtc { get; set; }
        public int Writes { get; private set; }

        public CachedManifest? Read() =>
            Json == null ? null : new CachedManifest(Json, FetchedUtc);

        public void Write(string json, DateTimeOffset fetchedUtc) {
            Json = json;
            FetchedUtc = fetchedUtc;
            Writes++;
        }
    }

    private sealed class ThrowingCache : IChannelCache {
        public CachedManifest? Read() => throw new UnauthorizedAccessException();
        public void Write(string json, DateTimeOffset fetchedUtc) =>
            throw new UnauthorizedAccessException();
    }
}
