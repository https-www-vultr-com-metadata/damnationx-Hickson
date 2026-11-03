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

fn helper_error(output: &std::process::Output) -> String {
    format!(
        "d2rtool exited with {}: {}",
        output.status,
        String::from_utf8_lossy(&output.stderr).trim()
    )
}

impl NativeStorage {
    pub fn build_version(&self) -> Result<String, String> {
        let bytes = fs::read(self.install.build_file()).map_err(|e| e.to_string())?;
        Ok(self.install.cache_key(&bytes))
    }
    pub fn new(
        game: impl AsRef<Path>,
        cache: impl AsRef<Path>,
        tool: impl AsRef<Path>,
    ) -> Result<Self, String> {
        let game = game
            .as_ref()
            .canonicalize()
            .map_err(|e| format!("Invalid D2R folder: {e}"))?;
        let install = Install::detect(&game).ok_or(
            "Choose your Diablo II: Resurrected install folder. A Battle.net install has \
             .build.info and Data; a Steam install has Data/.build.config.",
        )?;
        let tool = tool
            .as_ref()
            .canonicalize()
            .map_err(|e| format!("D2R archive helper unavailable: {e}"))?;
        fs::create_dir_all(cache.as_ref()).map_err(|e| e.to_string())?;
        let cache = cache.as_ref().canonicalize().map_err(|e| e.to_string())?;
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
                        .map_err(|e| e.to_string())?;
                    if !out.status.success() {
                        return Err(helper_error(&out));
                    }
                    let temp = file.with_extension("tsv.tmp");
                    fs::write(&temp, &out.stdout).map_err(|e| e.to_string())?;
                    fs::rename(&temp, &file).map_err(|e| e.to_string())?;
                    out.stdout
                }
                Err(e) => return Err(e.to_string()),
            };
            let contents = std::str::from_utf8(&contents).map_err(|e| e.to_string())?;
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
        fs::create_dir_all(&raw).map_err(|e| e.to_string())?;
        let mut child = self
            .command()
            .arg("batch")
            .arg(&raw)
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| e.to_string())?;
        let mut input = child.stdin.take().unwrap();
        // Drain stderr while writing stdin: a large batch of missing files can
        // otherwise fill both pipes and deadlock the helper.
        let writer = std::thread::spawn(move || -> Result<(), String> {
            for name in missing {
                writeln!(input, "data:{}", name.replace('/', "\\")).map_err(|e| e.to_string())?;
            }
            Ok(())
        });
        let out = child.wait_with_output().map_err(|e| e.to_string())?;
        let write_result = writer
            .join()
            .map_err(|_| "Archive input writer failed".to_string())?;
        // 3 reports individual missing entries; read() reports exactly which.
        if !out.status.success() && out.status.code() != Some(3) {
            return Err(helper_error(&out));
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
            .map_err(|e| e.to_string())?;
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
                .map_err(|e| e.to_string())?;
            if !out.status.success() {
                let _ = fs::remove_file(&temp);
                return Err(helper_error(&out));
            }
            // A failed helper must never leave a fresh-looking partial cache.
            if flat.exists() {
                fs::remove_file(&flat).map_err(|e| e.to_string())?;
            }
            fs::rename(temp, &flat).map_err(|e| e.to_string())?;
        }
        fs::read(flat).map_err(|e| e.to_string())
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

    #[test]
    fn cache_keys_are_portable_and_different_for_unknown_builds() {
        assert_eq!(cache_key(b"Version!STRING:0\n1.2.3\n"), "1.2.3");
        let first = cache_key(b"Branch!STRING:0\nfirst\n");
        assert!(first.starts_with("unknown-"));
        assert_ne!(first, cache_key(b"Branch!STRING:0\nsecond\n"));
        assert!(cache_key(b"Version!STRING:0\n?\n").starts_with("unknown-"));
    }
}
