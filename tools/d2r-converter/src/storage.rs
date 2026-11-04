//! Native access to the player's own CASC installation via the packaged d2rtool.
//!
//! Conversion operates only on extracted copies in the user cache. Archive paths
//! are validated before being passed to the helper or joined to that cache.
use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

pub struct NativeStorage {
    cache: PathBuf,
    tool: PathBuf,
    install: Install,
    listing: Option<BTreeMap<String, u64>>,
}

/// How an owned install stores its archives, and how CascLib is pointed at it.
#[derive(Debug, PartialEq, Eq)]
enum Install {
    /// Battle.net: `.build.info` beside `Data/`. CascLib opens the folder.
    BattleNet { game: PathBuf },
    /// Steam (since patch 3.2.0.1): a static store described by
    /// `Data/.build.config`, with no `.build.info`. CascLib is given that file:
    /// searching the folder itself only finds a lowercase `data`, which a
    /// case-sensitive volume (Linux, Steam Deck) spells `Data`.
    Steam { build_config: PathBuf },
}

impl Install {
    fn detect(game: &Path) -> Option<Self> {
        let data = [game.join("Data")]
            .into_iter()
            .find(|path| path.is_dir())
            .or_else(|| {
                fs::read_dir(game)
                    .ok()?
                    .take(256)
                    .flatten()
                    .find(|entry| {
                        entry.file_name().eq_ignore_ascii_case("data") && entry.path().is_dir()
                    })
                    .map(|entry| entry.path())
            })?;
        if game.join(".build.info").is_file() {
            return Some(Self::BattleNet {
                game: game.to_path_buf(),
            });
        }
        let build_config = data.join(".build.config");
        build_config
            .is_file()
            .then_some(Self::Steam { build_config })
    }

    /// The file that names the installed build; every patch changes it.
    fn build_file(&self) -> PathBuf {
        match self {
            Self::BattleNet { game } => game.join(".build.info"),
            Self::Steam { build_config } => build_config.clone(),
        }
    }

    fn cache_key(&self, build_file: &[u8]) -> String {
        match self {
            Self::BattleNet { .. } => cache_key(build_file),
            Self::Steam { .. } => steam_cache_key(build_file),
        }
    }

    /// The path CascLib opens: the Battle.net folder with the trailing separator
    /// CascLib expects even on Windows, or Steam's build config file.
    fn storage_arg(&self) -> std::ffi::OsString {
        match self {
            Self::BattleNet { game } => {
                let mut path = game.as_os_str().to_os_string();
                path.push(std::path::MAIN_SEPARATOR.to_string());
                // CascLib opens the first active row of .build.info. When
                // Battle.net lists several (a PTR or another product beside
                // D2R), name D2R's product code so the right build opens.
                let rows = fs::read(game.join(".build.info"))
                    .map(|bytes| build_info_rows(&String::from_utf8_lossy(&bytes)))
                    .unwrap_or_default();
                let active: Vec<_> = rows.iter().filter(|row| row.active).collect();
                if active.len() > 1 && active.iter().any(|row| row.product == D2R_PRODUCT) {
                    path.push(format!("*{D2R_PRODUCT}"));
                }
                path
            }
            Self::Steam { build_config } => build_config.as_os_str().to_os_string(),
        }
    }
}

fn relative_path(path: &str) -> Result<String, String> {
    let normalized = path.replace('\\', "/").to_lowercase();
    if !normalized.starts_with("data/")
        || normalized.contains(['\n', '\r', '\0', ':'])
        || normalized
            .split('/')
            .any(|s| s.is_empty() || s == "." || s == "..")
    {
        return Err(format!("Invalid archive path: {path}"));
    }
    Ok(normalized)
}

fn cache_key(info: &[u8]) -> String {
    let contents = String::from_utf8_lossy(info);
    let mut lines = contents.trim_start_matches('\u{feff}').lines();
    let header = lines.next().unwrap_or("");
    let row = lines.next().unwrap_or("");
    let version = header
        .split('|')
        .position(|v| v.split('!').next() == Some("Version"))
        .and_then(|i| row.split('|').nth(i))
        .unwrap_or("");
    if !version.is_empty()
        && version
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
        && version.as_bytes()[0].is_ascii_alphanumeric()
    {
        return version.to_owned();
    }
    // Stable hash of the actual build metadata keeps unknown builds separate.
    format!("unknown-{:016x}", fnv64(info))
}

fn fnv64(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for byte in bytes {
        hash = (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3);
    }
    hash
}

/// A Steam build is named by `build-name` in its `.build.config` when that is a
/// plain token. The 3.2.0.1 config has none, so the config's hash names it.
fn steam_cache_key(config: &[u8]) -> String {
    let contents = String::from_utf8_lossy(config);
    contents
        .lines()
        .filter_map(|line| line.split_once('='))
        .find(|(key, _)| key.trim() == "build-name")
        .map(|(_, value)| value.trim())
        .filter(|name| {
            name.bytes()
                .next()
                .is_some_and(|c| c.is_ascii_alphanumeric())
                && name
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
        })
        .map_or_else(|| format!("steam-{:016x}", fnv64(config)), str::to_owned)
}

/// Windows `canonicalize` returns verbatim `\\?\` paths, which Win32 does not
/// normalize. CascLib joins `game\` + `\.build.info` and d2rtool joins names with
/// `/`, so a verbatim storage path fails to open (CascLib error 2). This returns
/// the plain spelling when it names the same file.
fn plain_windows_path(path: &str) -> Option<String> {
    let rest = path.strip_prefix(r"\\?\")?;
    let plain = if let Some(unc) = rest.strip_prefix(r"UNC\") {
        format!(r"\\{unc}")
    } else {
        let bytes = rest.as_bytes();
        if bytes.len() < 3 || !bytes[0].is_ascii_alphabetic() || &bytes[1..3] != b":\\" {
            return None;
        }
        rest.to_owned()
    };
    // Keep the verbatim form where the plain path would mean something else.
    let reserved = |part: &str| {
        let stem = part.split('.').next().unwrap_or("").trim_end();
        let stem = stem.to_ascii_uppercase();
        matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL")
            || ((stem.starts_with("COM") || stem.starts_with("LPT"))
                && stem.len() == 4
                && stem.as_bytes()[3].is_ascii_digit())
    };
    let parts_ok = rest
        .split('\\')
        .skip(1)
        .filter(|part| !part.is_empty())
        .all(|part| !part.ends_with(['.', ' ']) && !part.contains('/') && !reserved(part));
    (plain.len() < 260 && parts_ok).then_some(plain)
}

fn plain_path(path: PathBuf) -> PathBuf {
    if cfg!(windows) {
        if let Some(plain) = path.to_str().and_then(plain_windows_path) {
            return PathBuf::from(plain);
        }
    }
    path
}

fn write_error(path: &Path, error: std::io::Error) -> String {
    format!("Cannot write {}: {error}", path.display())
}

/// What a CascLib open error (a Win32 error code on every platform) most likely
/// means for a player's D2R install, so the launcher can show a fix.
fn casc_open_hint(code: i64) -> &'static str {
    match code {
        2 | 3 => "the folder is missing D2R archive files; choose the folder that holds D2R.exe and Data, and let Battle.net or Steam finish installing",
        5 => "access was denied; check the folder's permissions and that antivirus is not blocking d2rtool.exe",
        32 | 33 => "the archives are in use; close D2R and let Battle.net or Steam finish any update, then retry",
        11 | 13 | 1392 => "the archives look damaged or mid-update; let Battle.net or Steam finish updating, or run Scan and Repair / Verify integrity",
        50 => "this D2R build stores its archives in a format the bundled reader does not support yet; report the D2R version",
        8 | 14 => "the system ran out of memory",
        _ => "check that the folder is a complete, up-to-date D2R install",
    }
}

/// Battle.net's product code for Diablo II: Resurrected.
const D2R_PRODUCT: &str = "osi";

/// One build row of a Battle.net `.build.info` (a `|`-separated table whose
/// header names columns like `Active!DEC:1`).
#[derive(Debug, Default, PartialEq, Eq)]
struct BuildRow {
    active: bool,
    product: String,
    build_key: String,
    cdn_key: String,
    version: String,
}

fn build_info_rows(text: &str) -> Vec<BuildRow> {
    let mut lines = text
        .trim_start_matches('\u{feff}')
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'));
    let Some(header) = lines.next() else {
        return Vec::new();
    };
    let columns: Vec<String> = header
        .split('|')
        .map(|column| column.split('!').next().unwrap_or("").trim().to_ascii_lowercase())
        .collect();
    let index = |name: &str| columns.iter().position(|column| column == name);
    let (active, product, build_key, cdn_key, version) = (
        index("active"),
        index("product"),
        index("build key"),
        index("cdn key"),
        index("version"),
    );
    lines
        .map(|line| {
            let fields: Vec<&str> = line.split('|').collect();
            let field = |at: Option<usize>| {
                at.and_then(|at| fields.get(at)).map_or(String::new(), |value| value.trim().to_string())
            };
            BuildRow {
                active: field(active) == "1",
                product: field(product),
                build_key: field(build_key).to_ascii_lowercase(),
                cdn_key: field(cdn_key).to_ascii_lowercase(),
                version: field(version),
            }
        })
        .collect()
}

/// Why CascLib most likely could not find a Battle.net install's files
/// (error 2), read from `.build.info` and the `Data` folder.
fn battle_net_diagnosis(game: &Path) -> Option<String> {
    let text = fs::read(game.join(".build.info")).ok()?;
    let rows = build_info_rows(&String::from_utf8_lossy(&text));
    if rows.is_empty() {
        return Some(".build.info lists no builds; run Scan and Repair in Battle.net".into());
    }
    let active: Vec<_> = rows.iter().filter(|row| row.active).collect();
    let Some(row) = active
        .iter()
        .find(|row| row.product == D2R_PRODUCT)
        .or_else(|| active.first())
    else {
        return Some(
            ".build.info marks no build as active, so Battle.net has an install or update pending; \
             open Battle.net, let Diablo II: Resurrected finish updating, then retry"
                .into(),
        );
    };
    let data = game.join("Data");
    for (what, key) in [("build config", &row.build_key), ("CDN config", &row.cdn_key)] {
        if key.len() < 4 || !key.is_ascii() {
            return Some(format!(".build.info has no valid {what} key; run Scan and Repair in Battle.net"));
        }
        let config = data.join("config").join(&key[0..2]).join(&key[2..4]).join(key);
        if !config.is_file() {
            return Some(format!(
                "Battle.net has not finished downloading D2R {} ({} is missing); open Battle.net and let it \
                 finish updating, or run Scan and Repair",
                if row.version.is_empty() { "build" } else { &row.version },
                config.display()
            ));
        }
    }
    let indices = fs::read_dir(data.join("data"))
        .map(|entries| {
            entries
                .flatten()
                .filter(|entry| {
                    entry.path().extension().is_some_and(|ext| ext.eq_ignore_ascii_case("idx"))
                })
                .count()
        })
        .unwrap_or(0);
    if indices == 0 {
        return Some(format!(
            "{} has no archive index files (*.idx); the install is incomplete, so run Scan and Repair in Battle.net",
            data.join("data").display()
        ));
    }
    None
}

/// One-line description of a failed d2rtool run, with a likely fix.
fn helper_error(output: &std::process::Output, install: &Install) -> String {
    let stderr = String::from_utf8_lossy(&output.stderr);
    let message = stderr.split_whitespace().collect::<Vec<_>>().join(" ");
    if let Some(code) = message
        .strip_prefix("open failed ")
        .and_then(|rest| rest.split_whitespace().next())
        .and_then(|code| code.parse::<i64>().ok())
    {
        let diagnosis = match install {
            Install::BattleNet { game } if code == 2 || code == 3 => battle_net_diagnosis(game),
            _ => None,
        };
        return format!(
            "d2rtool could not open the D2R archives (CascLib error {code}): {}",
            diagnosis.as_deref().unwrap_or(casc_open_hint(code))
        );
    }
    let detail = if message.is_empty() {
        "no error output".to_string()
    } else {
        message
    };
    format!("d2rtool {}: {detail}", describe_status(output.status))
}

/// `exited with exit code: 1`, plus the meaning of Windows crash and loader codes.
pub fn describe_status(status: std::process::ExitStatus) -> String {
    match status
        .code()
        .and_then(|code| windows_status_meaning(code as u32))
    {
        Some(meaning) => format!("exited with {status} ({meaning})"),
        None => format!("exited with {status}"),
    }
}

/// NTSTATUS values a process exits with when Windows itself stopped it.
pub fn windows_status_meaning(code: u32) -> Option<&'static str> {
    Some(match code {
        0xC000_0135 => "a required DLL was not found; install the Microsoft Visual C++ 2015-2022 x64 Redistributable",
        0xC000_007B => "a DLL is for the wrong architecture or damaged; reinstall the Microsoft Visual C++ 2015-2022 x64 Redistributable",
        0xC000_0139 | 0xC000_0142 => "a required DLL failed to load",
        0xC000_0005 => "it crashed with an access violation",
        0xC000_00FD => "it crashed with a stack overflow",
        0xC000_0409 => "it aborted on an internal check",
        0xC000_0017 | 0xC000_009A => "the system ran out of memory",
        0xC000_013A => "it was interrupted",
        0xC000_0022 => "access was denied; antivirus may be blocking it",
        _ => return None,
    })
}

impl NativeStorage {
    /// The helper could not be started at all: usually quarantined by antivirus
    /// or missing because the package was not fully extracted.
    fn spawn_error(&self, error: std::io::Error) -> String {
        format!(
            "Cannot start the D2R archive helper {}: {error}. Re-extract the full package; if antivirus quarantined d2rtool.exe, restore it or allow it",
            self.tool.display()
        )
    }
}

impl NativeStorage {
    pub fn build_version(&self) -> Result<String, String> {
        let file = self.install.build_file();
        let bytes = fs::read(&file)
            .map_err(|e| format!("Cannot read the D2R build file {}: {e}", file.display()))?;
        Ok(self.install.cache_key(&bytes))
    }
    pub fn new(
        game: impl AsRef<Path>,
        cache: impl AsRef<Path>,
        tool: impl AsRef<Path>,
    ) -> Result<Self, String> {
        let game = plain_path(
            game.as_ref()
                .canonicalize()
                .map_err(|e| format!("Invalid D2R folder: {e}"))?,
        );
        let install = Install::detect(&game).ok_or(
            "Choose your Diablo II: Resurrected install folder. A Battle.net install has \
             .build.info and Data; a Steam install has Data/.build.config.",
        )?;
        let tool = plain_path(
            tool.as_ref()
                .canonicalize()
                .map_err(|e| format!("D2R archive helper unavailable: {e}"))?,
        );
        let cache_error = |e: std::io::Error| {
            format!(
                "Cannot use the conversion cache {}: {e}",
                cache.as_ref().display()
            )
        };
        fs::create_dir_all(cache.as_ref()).map_err(cache_error)?;
        let cache = plain_path(cache.as_ref().canonicalize().map_err(cache_error)?);
        if cache.starts_with(&game) {
            return Err("The conversion cache must be outside the D2R installation".into());
        }
        Ok(Self {
            cache,
            tool,
            install,
            listing: None,
        })
    }

    fn command(&self) -> Command {
        let mut command = Command::new(&self.tool);
        command.arg(self.install.storage_arg());
        command
    }

    pub fn listing(&mut self) -> Result<&BTreeMap<String, u64>, String> {
        if self.listing.is_none() {
            let file = self
                .cache
                .join(format!("listfile-{}.tsv", self.build_version()?));
            let contents = match fs::read(&file) {
                Ok(contents) => contents,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                    let out = self
                        .command()
                        .arg("list")
                        .output()
                        .map_err(|e| self.spawn_error(e))?;
                    if !out.status.success() {
                        return Err(helper_error(&out, &self.install));
                    }
                    let temp = file.with_extension("tsv.tmp");
                    fs::write(&temp, &out.stdout).map_err(|e| write_error(&temp, e))?;
                    fs::rename(&temp, &file).map_err(|e| write_error(&file, e))?;
                    out.stdout
                }
                Err(e) => return Err(format!("Cannot read {}: {e}", file.display())),
            };
            let contents = std::str::from_utf8(&contents)
                .map_err(|e| format!("The archive listing {} is not UTF-8: {e}", file.display()))?;
            let mut listing = BTreeMap::new();
            for line in contents.lines() {
                if let Some((size, name)) = line.split_once('\t') {
                    if let (Ok(size), Some(name)) = (size.parse(), name.strip_prefix("data:")) {
                        if let Ok(name) = relative_path(name) {
                            listing.insert(name, size);
                        }
                    }
                }
            }
            self.listing = Some(listing);
        }
        Ok(self.listing.as_ref().unwrap())
    }

    pub fn fetch(&self, paths: &[String]) -> Result<(), String> {
        let raw = self.cache.join("raw");
        let mut missing = Vec::new();
        for path in paths {
            let name = relative_path(path)?;
            if !raw.join(&name).is_file() {
                missing.push(name);
            }
        }
        if missing.is_empty() {
            return Ok(());
        }
        fs::create_dir_all(&raw).map_err(|e| write_error(&raw, e))?;
        let mut child = self
            .command()
            .arg("batch")
            .arg(&raw)
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| self.spawn_error(e))?;
        let mut input = child.stdin.take().unwrap();
        // Drain stderr while writing stdin: a large batch of missing files can
        // otherwise fill both pipes and deadlock the helper.
        let writer = std::thread::spawn(move || -> Result<(), String> {
            for name in missing {
                writeln!(input, "data:{}", name.replace('/', "\\")).map_err(|e| e.to_string())?;
            }
            Ok(())
        });
        let out = child
            .wait_with_output()
            .map_err(|e| format!("The D2R archive helper did not finish: {e}"))?;
        let write_result = writer
            .join()
            .map_err(|_| "Archive input writer failed".to_string())?;
        // 3 reports individual missing entries; read() reports exactly which.
        if !out.status.success() && out.status.code() != Some(3) {
            return Err(helper_error(&out, &self.install));
        }
        write_result?;
        Ok(())
    }

    pub fn read(&self, path: &str) -> Result<Vec<u8>, String> {
        let name = relative_path(path)?;
        self.fetch(std::slice::from_ref(&name))?;
        fs::read(self.cache.join("raw").join(&name))
            .map_err(|e| format!("Could not read {name}: {e}"))
    }

    pub fn granny(&self, path: &str) -> Result<Vec<u8>, String> {
        let name = relative_path(path)?;
        self.fetch(std::slice::from_ref(&name))?;
        let source = self.cache.join("raw").join(&name);
        let mut flat = source.as_os_str().to_os_string();
        flat.push(".grfl");
        let flat = PathBuf::from(flat);
        let source_time = fs::metadata(&source)
            .and_then(|m| m.modified())
            .map_err(|e| format!("Cannot read {}: {e}", source.display()))?;
        let stale = fs::metadata(&flat)
            .and_then(|m| m.modified())
            .map_or(true, |t| t < source_time);
        if stale {
            let temp = flat.with_extension("grfl.tmp");
            let out = Command::new(&self.tool)
                .arg("-")
                .arg("gr2flat")
                .arg(&source)
                .arg(&temp)
                .output()
                .map_err(|e| self.spawn_error(e))?;
            if !out.status.success() {
                let _ = fs::remove_file(&temp);
                return Err(helper_error(&out, &self.install));
            }
            // A failed helper must never leave a fresh-looking partial cache.
            if flat.exists() {
                fs::remove_file(&flat).map_err(|e| write_error(&flat, e))?;
            }
            fs::rename(temp, &flat).map_err(|e| write_error(&flat, e))?;
        }
        fs::read(&flat).map_err(|e| format!("Cannot read {}: {e}", flat.display()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A scratch folder removed when the test ends.
    struct TempDir(PathBuf);
    impl TempDir {
        fn new(name: &str) -> Self {
            let nanos = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let path = std::env::temp_dir()
                .join(format!("d2r-storage-{name}-{}-{nanos}", std::process::id()));
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn invalid_archive_paths_cannot_escape_cache_or_inject_batch_commands() {
        for path in [
            "../data/a",
            "data/../x",
            "data//x",
            "data/x\ndata:y",
            "data/x:y",
        ] {
            assert!(relative_path(path).is_err(), "{path}");
        }
        assert_eq!(
            relative_path("DATA\\HD\\A.texture").unwrap(),
            "data/hd/a.texture"
        );
    }
    fn write(path: &Path, contents: &str) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, contents).unwrap();
    }

    #[test]
    fn battle_net_and_steam_installs_are_detected_and_partial_ones_refused() {
        let temp = TempDir::new("detect");
        let battle_net = temp.path().join("bnet");
        write(
            &battle_net.join(".build.info"),
            "Version!STRING:0\n1.6.80273\n",
        );
        fs::create_dir_all(battle_net.join("Data/indices")).unwrap();
        assert_eq!(
            Install::detect(&battle_net),
            Some(Install::BattleNet {
                game: battle_net.clone()
            })
        );
        let steam = temp.path().join("steam");
        write(&steam.join("Data/.build.config"), "root = 00\n");
        write(&steam.join("Data/00-00000001.data"), "");
        let config = steam.join("Data").join(".build.config");
        assert_eq!(
            Install::detect(&steam),
            Some(Install::Steam {
                build_config: config.clone()
            })
        );
        // CascLib opens Battle.net by folder, Steam by its build config file.
        let mut folder = battle_net.clone().into_os_string();
        folder.push(std::path::MAIN_SEPARATOR.to_string());
        assert_eq!(Install::detect(&battle_net).unwrap().storage_arg(), folder);
        assert_eq!(
            Install::detect(&steam).unwrap().storage_arg(),
            config.into_os_string()
        );
        let partial = temp.path().join("archives-only");
        fs::create_dir_all(partial.join("Data")).unwrap();
        assert_eq!(Install::detect(&partial), None);
        write(&partial.join("Data/00-00000001.data"), "");
        assert_eq!(Install::detect(&partial), None);
        write(&partial.join(".build.config"), "root = 00\n");
        assert_eq!(Install::detect(&partial), None);
    }

    #[test]
    fn steam_builds_are_named_by_build_name_or_config_hash() {
        let config = b"# Build Configuration\nroot = bf9064519cc56ba5fe197ce27e4729a1\n";
        let key = steam_cache_key(config);
        assert!(key.starts_with("steam-") && key.len() == 22, "{key}");
        assert_eq!(steam_cache_key(config), key);
        assert_ne!(steam_cache_key(b"root = 0123\n"), key);
        assert_eq!(
            steam_cache_key(b"build-name = D2R-3.2.0.1_93236\nroot = 00\n"),
            "D2R-3.2.0.1_93236"
        );
        // A name that is not a plain file-name token never names a cache file.
        assert!(steam_cache_key(b"build-name = ../../x\n").starts_with("steam-"));
        assert!(steam_cache_key(b"build-name = \n").starts_with("steam-"));
    }

    /// The helper receives Steam's build config as its storage argument and the
    /// listing is cached under the Steam build's name.
    #[cfg(unix)]
    #[test]
    fn steam_listing_opens_casc_by_build_config() {
        use std::os::unix::fs::PermissionsExt;
        let temp = TempDir::new("listing");
        let steam = temp.path().join("Diablo II Resurrected");
        write(&steam.join("Data/.build.config"), "root = 00\n");
        let log = temp.path().join("helper-args.txt");
        let tool = temp.path().join("d2rtool");
        write(
            &tool,
            &format!(
                "#!/bin/sh\nprintf '%s\\n' \"$@\" > '{}'\nprintf '12\\tdata:data\\\\global\\\\excel\\\\levels.txt\\n7\\tindex\\n'\n",
                log.display()
            ),
        );
        fs::set_permissions(&tool, fs::Permissions::from_mode(0o755)).unwrap();
        let mut storage = NativeStorage::new(&steam, temp.path().join("cache"), &tool).unwrap();
        let listing = storage.listing().unwrap().clone();
        assert_eq!(
            listing.into_iter().collect::<Vec<_>>(),
            vec![("data/global/excel/levels.txt".to_string(), 12)]
        );
        let config = steam
            .canonicalize()
            .unwrap()
            .join("Data")
            .join(".build.config");
        assert_eq!(
            fs::read_to_string(&log).unwrap(),
            format!("{}\nlist\n", config.display())
        );
        let key = storage.build_version().unwrap();
        assert!(temp
            .path()
            .join("cache")
            .canonicalize()
            .unwrap()
            .join(format!("listfile-{key}.tsv"))
            .is_file());
    }

    /// Helper failures reach the launcher as one line that names the cause.
    #[cfg(unix)]
    #[test]
    fn helper_failures_are_one_explained_line() {
        use std::os::unix::fs::PermissionsExt;
        let temp = TempDir::new("helper-errors");
        let game = temp.path().join("D2R");
        write(&game.join(".build.info"), "Version!STRING:0\n1.6.80273\n");
        fs::create_dir_all(game.join("Data")).unwrap();
        let tool = temp.path().join("d2rtool");
        let mut errors = Vec::new();
        for script in [
            "echo 'open failed 2' >&2; exit 1",
            "echo 'open failed 32' >&2; exit 1",
            "echo 'boom' >&2; echo 'second line' >&2; exit 4",
            "exit 9",
        ] {
            write(&tool, &format!("#!/bin/sh\n{script}\n"));
            fs::set_permissions(&tool, fs::Permissions::from_mode(0o755)).unwrap();
            let mut storage = NativeStorage::new(&game, temp.path().join("cache"), &tool).unwrap();
            errors.push(storage.listing().unwrap_err());
        }
        assert!(errors[0].starts_with("d2rtool could not open the D2R archives (CascLib error 2)"));
        assert!(errors[1].contains("close D2R"), "{}", errors[1]);
        assert_eq!(
            errors[2],
            "d2rtool exited with exit status: 4: boom second line"
        );
        assert_eq!(
            errors[3],
            "d2rtool exited with exit status: 9: no error output"
        );
        assert!(errors.iter().all(|e| !e.contains('\n')));
        fs::remove_file(&tool).unwrap();
        let missing = NativeStorage::new(&game, temp.path().join("cache"), &tool);
        assert!(missing.is_err_and(|e| e.starts_with("D2R archive helper unavailable")));
    }


    /// CascLib error 2 on a Battle.net install names what is missing.
    #[test]
    fn battle_net_open_errors_name_the_missing_piece() {
        let temp = TempDir::new("diagnosis");
        let game = temp.path().join("D2R");
        let header = "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16|Version!STRING:0|Product!STRING:0";
        let build = "aabbccddeeff00112233445566778899";
        let cdn = "0123456789abcdef0123456789abcdef";
        write(
            &game.join(".build.info"),
            &format!("{header}\nus|0|{build}|{cdn}|3.3.93847|osi\n"),
        );
        fs::create_dir_all(game.join("Data/data")).unwrap();
        assert!(battle_net_diagnosis(&game).unwrap().contains("no build as active"));

        write(
            &game.join(".build.info"),
            &format!("{header}\nus|1|{build}|{cdn}|3.3.93847|osi\n"),
        );
        let missing = battle_net_diagnosis(&game).unwrap();
        assert!(missing.contains("3.3.93847") && missing.contains(build), "{missing}");

        for key in [build, cdn] {
            write(
                &game.join("Data/config").join(&key[0..2]).join(&key[2..4]).join(key),
                "",
            );
        }
        assert!(battle_net_diagnosis(&game).unwrap().contains("*.idx"));
        write(&game.join("Data/data/0000000001.idx"), "");
        assert_eq!(battle_net_diagnosis(&game), None);

        // One active row opens as before; several name D2R's product.
        let install = Install::detect(&game).unwrap();
        assert!(!install.storage_arg().to_string_lossy().contains('*'));
        write(
            &game.join(".build.info"),
            &format!("{header}\nus|1|{cdn}|{cdn}|9.9|osib\nus|1|{build}|{cdn}|3.3.93847|osi\n"),
        );
        assert!(install.storage_arg().to_string_lossy().ends_with("*osi"));
        assert_eq!(battle_net_diagnosis(&game), None);
    }
    #[test]
    fn windows_loader_and_crash_codes_are_explained() {
        assert!(windows_status_meaning(0xC000_0135)
            .unwrap()
            .contains("Visual C++"));
        assert!(windows_status_meaning(1).is_none());
    }

    #[test]
    fn verbatim_windows_paths_become_plain_only_when_equivalent() {
        assert_eq!(
            plain_windows_path(r"\\?\E:\_stuff\diablo 1 (trygentic) (PC)\Diablo II Resurrected")
                .as_deref(),
            Some(r"E:\_stuff\diablo 1 (trygentic) (PC)\Diablo II Resurrected")
        );
        assert_eq!(
            plain_windows_path(r"\\?\UNC\nas\games\D2R").as_deref(),
            Some(r"\\nas\games\D2R")
        );
        for kept in [
            r"C:\Games\D2R".to_string(),
            r"\\?\Volume{1234}\D2R".to_string(),
            r"\\?\C:\Games\con\D2R".to_string(),
            r"\\?\C:\Games\COM1.txt".to_string(),
            r"\\?\C:\Games\trailing.\D2R".to_string(),
            format!(r"\\?\C:\{}", "a".repeat(300)),
        ] {
            assert_eq!(plain_windows_path(&kept), None, "{kept}");
        }
    }

    #[test]
    fn cache_keys_are_portable_and_different_for_unknown_builds() {
        assert_eq!(cache_key(b"Version!STRING:0\n1.2.3\n"), "1.2.3");
        let first = cache_key(b"Branch!STRING:0\nfirst\n");
        assert!(first.starts_with("unknown-"));
        assert_ne!(first, cache_key(b"Branch!STRING:0\nsecond\n"));
        assert!(cache_key(b"Version!STRING:0\n?\n").starts_with("unknown-"));
    }
}
