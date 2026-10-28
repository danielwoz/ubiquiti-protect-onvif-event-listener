# onvif-recorder APT repository (gh-pages)

This directory is a template for the contents of the `gh-pages` branch, which
is served as an APT repository at:

    https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/

## Bootstrap (one-time)

From `main`, copy the scaffolding into a fresh `gh-pages` branch:

```bash
git worktree add /tmp/gh-pages-bootstrap --orphan gh-pages
cp -r gh-pages/* /tmp/gh-pages-bootstrap/
cp -r gh-pages/.gitignore /tmp/gh-pages-bootstrap/ 2>/dev/null || true

# Export the public half of the release-signing key.
gpg --armor --export <KEYID> > /tmp/gh-pages-bootstrap/onvif-recorder.gpg

cd /tmp/gh-pages-bootstrap
git add -A
git commit -m "Bootstrap gh-pages apt repo (stable/rc/early-access)"
git push -u origin gh-pages
cd -
git worktree remove /tmp/gh-pages-bootstrap
```

Enable GitHub Pages for the repo: Settings → Pages → Source = `gh-pages` branch,
root directory.

## Layout after first release

```
/                                       apt root
  conf/
    distributions                       suite definitions (stable/rc/early-access)
    options
  dists/
    stable/         {InRelease, Release, Release.gpg, main/binary-arm64/Packages.gz}
    rc/             {…}
    early-access/   {…}
  pool/main/o/onvif-recorder/onvif-recorder_*_arm64.deb
  onvif-recorder.gpg                    public key (ASCII-armoured)
  install.sh                            one-line installer (see below)
  channels.json                         release channel manifest (see below)
```

## Channel manifest (`channels.json`)

`channels.json` lists the release channels with their labels and the
onvif-recorder version currently in each suite. The Windows installer reads
it to fill its channel drop-down.

- **Source.** `channels.meta.json` in this directory on `main` holds the
  ids, aliases, labels, descriptions, order and default.
  `scripts/gen_channels_json.py` adds each suite's version (from
  `dists/<id>/main/binary-arm64/Packages`) and `updated_at` (the suite's
  `Release` `Date:`). A suite with no build gets `"version": null`.
- **Publishing.** `release-deb.yml` and `promote.yml` regenerate it in the
  same gh-pages commit as their reprepro change, always with the generator
  and metadata from `main`. `channels-manifest.yml` regenerates it on a push
  to `main` that touches the metadata, the generator or `install.sh`, or
  when run by hand, and commits only when it or `install.sh` changed. All
  three (release-deb only in its short `publish-apt` job) share the
  `gh-pages-publish` concurrency group. Never edit `channels.json` on
  gh-pages by hand.
- **Latest = early-access.** "Latest (alpha)" is the label of the
  `early-access` suite, with `latest` as an alias. The suite, the channel
  file on the router and the admin page keep the id `early-access`.
- **Changing labels or the default.** Edit `channels.meta.json` on `main`
  and push; `channels-manifest.yml` publishes it. The installer embeds the
  same file as its offline list, so the next installer build picks it up
  too.
- **Adding a channel.** Add the suite to `conf/distributions` and to
  `channels.meta.json` only after the router side accepts it:
  `debian/install-apt-source.sh`, `debian/detect-channel.sh` and the admin
  page only know `stable`, `rc` and `early-access`.

### Schema 1

```json
{
  "schema": 1,
  "generated_at": "2026-10-28T06:00:00Z",
  "default": "early-access",
  "channels": [
    { "id": "early-access", "aliases": ["latest"], "label": "Latest (alpha)",
      "description": "…", "version": "1.6.32", "updated_at": "2026-10-22T19:37:09Z" }
  ],
  "installer": { "min_version": null, "download_url": "https://github.com/…/releases/latest" }
}
```

What readers rely on, so changes must keep it:

- `schema` is the integer `1`. A breaking change ships as
  `channels-v2.json` while `channels.json` keeps being generated. Adding
  fields never changes the schema; readers ignore unknown fields.
- `id` matches `^[a-z][a-z0-9-]{0,30}[a-z0-9]$`, because the installer
  writes it into a root shell script on the router. The generator refuses
  any other id; the installer drops it.
- `label` is at most 40 characters and `description` at most 200, single
  line.
- `version` is a dpkg version or `null` (no build in that suite yet).
- `default` is one of the ids.
- `installer.min_version`: installers older than this show a banner
  pointing at `installer.download_url` (https only). It never blocks.
- The file stays under 64 KiB.

Check the published copy with:

```bash
curl -s https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/channels.json \
  | python3 -m json.tool
```

GitHub Pages serves it with `cache-control: max-age=600`, so a new version
can take about ten minutes plus the Pages build to show up.

## Installer (`install.sh`)

`install.sh` at the root of gh-pages is the installer behind
`curl -fsSL …/install.sh | sh`. Its source is `install.sh` in this directory
on `main`; the package also ships that file as
`/usr/share/onvif-recorder/install.sh` for the recovery layer.

- **Publishing.** Every gh-pages commit made by a workflow copies `main`'s
  `gh-pages/install.sh` to `install.sh` with mode 0755: `release-deb.yml`
  and `promote.yml` in their reprepro commit, and `channels-manifest.yml`
  on a push to `main` that touches it, or when run by hand, in one commit
  with `channels.json` made only when either changed. It always comes from
  `main`, never from a tag, so re-running an old release does not bring
  back an old installer.
- **Syntax gate.** A copy that fails `sh -n` is not published: the copy
  already on gh-pages stays and the run shows an error annotation
  (`channels-manifest.yml` also fails the run). `//test:test_debian_scripts`
  runs the same check in CI.
- Never edit `install.sh` on gh-pages by hand; change it here and push.

Check the published copy against `main` with:

```bash
git fetch origin main
curl -fsS https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/install.sh \
  | diff - <(git show origin/main:gh-pages/install.sh) && echo "install.sh matches main"
```

## Secrets

The release workflow needs two GitHub Actions secrets:

- `GPG_SIGNING_KEY` — ASCII-armoured *private* half of the release signing key.
  Generate with `gpg --armor --export-secret-keys <KEYID>`.
- `GITHUB_TOKEN` is provided automatically.
