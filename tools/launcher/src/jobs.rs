//! Desktop workers. No subprocess, archive extraction, directory traversal or
//! blocking join runs on egui's thread. A completed staging tree is published
//! only after validation; cancellation before commit leaves the old tree intact.
use crate::assets::{self, AssetPaths, AssetReadiness};
use crate::d2r_cache::{self, CacheDecision, ImportStamp};
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::ffi::OsString;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const MAX_FILES: usize = 200_000;
const MAX_BYTES: u64 = 256 * 1024 * 1024 * 1024;
const MAX_LINE: usize = 4096;
const LOG_TAIL: usize = 64;
const MAX_REFERENCE_DEPTH: usize = 64;
/// Shown when the chosen folder is not a D2R install the importers can read.
const D2R_INSTALL_REQUIRED: &str = "Choose your Diablo II: Resurrected install folder. \
     A Battle.net install has .build.info and Data/indices; a Steam install has Data/.build.config.";

#[derive(Clone, Debug)]
pub struct D2rImportRequest {
    pub game_dir: PathBuf,
    /// Cache parent, not its glb child. Conversion work is retained separately
    /// from the published glb tree until the whole import validates.
    pub cache_dir: PathBuf,
    pub importer_dir: PathBuf,
    /// Full converter output for diagnosing failures; None keeps it in memory only.
    pub log_file: Option<PathBuf>,
}

#[derive(Clone, Debug)]
pub struct D2rSoundImportRequest {
    pub program: PathBuf,
    pub game_dir: PathBuf,
    /// The native preference folder's existing mods directory.
    pub destination: PathBuf,
    pub classic_runtime: bool,
    pub data_dir: Option<PathBuf>,
    /// Import record that lets an unchanged repeat import be skipped.
    pub stamp: Option<PathBuf>,
}

#[derive(Clone, Debug)]
pub enum JobRequest {
    Inspect(AssetPaths),
    InstallHd {
        source: PathBuf,
        destination: PathBuf,
        overlay: bool,
    },
    /// Migrate an already selected, validated HD reference into regular files.
    /// New folder/ZIP imports always use the stricter InstallHd path.
    CopyHdReference {
        source: PathBuf,
        destination: PathBuf,
    },
    ImportD2r(D2rImportRequest),
    ImportD2rSounds(D2rSoundImportRequest),
    /// Remove the launcher-created D2R art cache and sound packs only.
    ClearD2r {
        cache_dir: PathBuf,
        sound_mods: PathBuf,
        protected: Vec<PathBuf>,
    },
}

#[derive(Clone, Debug)]
pub enum JobOutcome {
    Inspected(AssetReadiness),
    InstalledHd(PathBuf),
    /// Published folder, and whether an unchanged previous import was reused.
    ImportedD2r(PathBuf, bool),
    ImportedD2rSounds(PathBuf, bool),
    ClearedD2r(Vec<PathBuf>),
}

#[derive(Clone, Debug)]
pub enum JobEvent {
    /// Actual bytes copied or completed pipeline stages. None means indeterminate.
    Progress {
        phase: String,
        done: u64,
        total: Option<u64>,
    },
    Log(String),
    Finished(Result<JobOutcome, String>),
}

pub struct JobHandle {
    events: mpsc::Receiver<JobEvent>,
    cancel: Arc<AtomicBool>,
    finished: Arc<AtomicBool>,
}

impl JobHandle {
    pub fn spawn(request: JobRequest) -> Self {
        let (sender, events) = mpsc::sync_channel(128);
        let cancel = Arc::new(AtomicBool::new(false));
        let finished = Arc::new(AtomicBool::new(false));
        let reporter = Reporter {
            sender,
            cancel: cancel.clone(),
            dropped: Arc::new(AtomicU64::new(0)),
        };
        let done = finished.clone();
        thread::spawn(move || {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match request {
                JobRequest::Inspect(paths) => {
                    reporter.check()?;
                    Ok(JobOutcome::Inspected(assets::inspect(&paths)))
                }
                JobRequest::InstallHd {
                    source,
                    destination,
                    overlay,
                } => install_hd(&source, &destination, overlay, &reporter)
                    .map(JobOutcome::InstalledHd),
                JobRequest::CopyHdReference {
                    source,
                    destination,
                } => {
                    copy_hd_reference(&source, &destination, &reporter).map(JobOutcome::InstalledHd)
                }
                JobRequest::ImportD2r(request) => import_d2r_cached(&request, &reporter)
                    .map(|(path, reused)| JobOutcome::ImportedD2r(path, reused)),
                JobRequest::ImportD2rSounds(request) => {
                    import_d2r_sounds_cached(&request, &reporter)
                        .map(|(path, reused)| JobOutcome::ImportedD2rSounds(path, reused))
                }
                JobRequest::ClearD2r {
                    cache_dir,
                    sound_mods,
                    protected,
                } => {
                    reporter.progress("Clearing imported D2R assets", 0, None);
                    d2r_cache::clear_import(&cache_dir, &sound_mods, &protected).map(|removed| {
                        for path in &removed {
                            reporter.log(format!("Removed {}", path.display()));
                        }
                        JobOutcome::ClearedD2r(removed)
                    })
                }
            }))
            .unwrap_or_else(|_| {
                Err(
                    "Asset worker failed unexpectedly; the existing installation was preserved"
                        .into(),
                )
            });
            let dropped = reporter.dropped.load(Ordering::Relaxed);
            if dropped != 0 {
                reporter.log(format!(
                    "{dropped} live log lines omitted while the UI queue was full"
                ));
            }
            done.store(true, Ordering::Release);
            // Terminal results are never dropped. Dropping the UI handle closes
            // this channel and cancels the worker without waiting on the UI.
            let _ = reporter.sender.send(JobEvent::Finished(result));
        });
        Self {
            events,
            cancel,
            finished,
        }
    }

    pub fn poll(&self) -> Vec<JobEvent> {
        self.events.try_iter().take(128).collect()
    }
    pub fn cancel(&self) {
        self.cancel.store(true, Ordering::Release);
    }
    pub fn is_finished(&self) -> bool {
        self.finished.load(Ordering::Acquire)
    }
}

impl Drop for JobHandle {
    fn drop(&mut self) {
        self.cancel();
    }
}

#[derive(Clone)]
struct Reporter {
    sender: mpsc::SyncSender<JobEvent>,
    cancel: Arc<AtomicBool>,
    dropped: Arc<AtomicU64>,
}

impl Reporter {
    fn check(&self) -> Result<(), String> {
        if self.cancel.load(Ordering::Acquire) {
            Err("Cancelled; the existing installation was preserved".into())
        } else {
            Ok(())
        }
    }
    fn log(&self, line: impl Into<String>) {
        let line: String = line.into().chars().take(MAX_LINE).collect();
        if self.sender.try_send(JobEvent::Log(line)).is_err() {
            self.dropped.fetch_add(1, Ordering::Relaxed);
        }
    }
    fn progress(&self, phase: &str, done: u64, total: Option<u64>) {
        let _ = self.sender.try_send(JobEvent::Progress {
            phase: phase.into(),
            done,
            total,
        });
    }
}

struct Stage {
    directory: PathBuf,
    preserve: bool,
    _lock: File,
}

impl Stage {
    fn new(destination: &Path) -> Result<Self, String> {
        let parent = destination
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .ok_or("Installation needs an absolute destination with a parent")?;
        if !destination.is_absolute() {
            return Err("Managed installation destination must be absolute".into());
        }
        fs::create_dir_all(parent).map_err(|e| format!("Cannot create cache folder: {e}"))?;
        if fs::symlink_metadata(destination)
            .is_ok_and(|m| m.file_type().is_symlink() || !m.is_dir())
        {
            return Err("Existing installation must be a directory, not a symlink or file".into());
        }
        let lock = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(parent.join(".damnationx-install.lock"))
            .map_err(|e| e.to_string())?;
        lock.try_lock()
            .map_err(|e| format!("Another asset install is using this cache: {e}"))?;
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_nanos();
        for _ in 0..32 {
            let directory = parent.join(format!(
                ".damnationx-stage-{}-{stamp}-{}",
                std::process::id(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            ));
            match fs::create_dir(&directory) {
                Ok(()) => {
                    return Ok(Self {
                        directory,
                        preserve: false,
                        _lock: lock,
                    })
                }
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(format!("Cannot create staging directory: {e}")),
            }
        }
        Err("Cannot allocate a unique staging directory".into())
    }

    fn publish(
        &mut self,
        payload: &Path,
        destination: &Path,
        reporter: &Reporter,
    ) -> Result<(), String> {
        reporter.check()?;
        let previous = self.directory.join("previous");
        let existed = destination.exists();
        if existed {
            fs::rename(destination, &previous)
                .map_err(|e| format!("Cannot move the existing installation aside: {e}"))?;
        }
        // A tiny commit region intentionally ignores cancellation: either the
        // validated new tree or the restored previous tree occupies destination.
        if let Err(error) = fs::rename(payload, destination) {
            if existed {
                if let Err(restore) = fs::rename(&previous, destination) {
                    self.preserve = true;
                    return Err(format!("Publish failed ({error}); automatic restore failed ({restore}). Previous installation remains at {}", previous.display()));
                }
            }
            return Err(format!(
                "Publish failed; previous installation restored: {error}"
            ));
        }
        reporter.log(format!("Installed {}", destination.display()));
        Ok(())
    }
}

impl Drop for Stage {
    fn drop(&mut self) {
        if !self.preserve {
            let _ = fs::remove_dir_all(&self.directory);
        }
        // A duplicated/inherited descriptor can outlive this owner. Closing
        // only our descriptor would keep its shared lock held until that copy
        // closes, so release ownership explicitly after staging cleanup.
        let _ = self._lock.unlock();
    }
}

#[derive(Clone)]
struct CopyFile {
    source: PathBuf,
    relative: PathBuf,
    bytes: u64,
}

fn inventory(root: &Path, reporter: &Reporter) -> Result<Vec<CopyFile>, String> {
    let mut stack = vec![root.to_path_buf()];
    let mut files = Vec::new();
    let mut total = 0u64;
    let mut nodes = 0usize;
    let mut portable_names = BTreeSet::new();
    while let Some(directory) = stack.pop() {
        reporter.check()?;
        let meta = fs::symlink_metadata(&directory).map_err(|e| e.to_string())?;
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err(format!(
                "Folder contains a link or special entry: {}",
                directory.display()
            ));
        }
        for entry in fs::read_dir(&directory).map_err(|e| e.to_string())? {
            reporter.check()?;
            let entry = entry.map_err(|e| e.to_string())?;
            nodes += 1;
            if nodes > MAX_FILES {
                return Err("Asset folder contains too many entries".into());
            }
            let path = entry.path();
            let meta = fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
            if meta.file_type().is_symlink() {
                return Err(format!("Symlinks are not imported: {}", path.display()));
            }
            let relative = path
                .strip_prefix(root)
                .map_err(|e| e.to_string())?
                .to_path_buf();
            assets::relative_path(&relative.to_string_lossy())?;
            if !portable_names.insert(relative.to_string_lossy().to_lowercase()) {
                return Err(
                    "Folder has filenames that collide on a case-insensitive filesystem".into(),
                );
            }
            if meta.is_dir() {
                stack.push(path);
            } else if meta.is_file() {
                total = total.checked_add(meta.len()).ok_or("Asset size overflow")?;
                if total > MAX_BYTES {
                    return Err("Asset import exceeds the 256 GiB limit".into());
                }
                files.push(CopyFile {
                    source: path,
                    relative,
                    bytes: meta.len(),
                });
            } else {
                return Err(format!(
                    "Special files are not imported: {}",
                    path.display()
                ));
            }
        }
    }
    files.sort_by(|a, b| a.relative.cmp(&b.relative));
    Ok(files)
}

struct ReferenceInventory<'a> {
    reporter: &'a Reporter,
    files: Vec<CopyFile>,
    bytes: u64,
    nodes: usize,
    names: BTreeSet<String>,
    ancestors: BTreeSet<PathBuf>,
    stage: PathBuf,
}

impl ReferenceInventory<'_> {
    fn visit(&mut self, directory: &Path, relative: &Path, depth: usize) -> Result<(), String> {
        self.reporter.check()?;
        if depth > MAX_REFERENCE_DEPTH {
            return Err("Existing HD reference exceeds 64 directory levels".into());
        }
        let canonical = directory
            .canonicalize()
            .map_err(|e| format!("Cannot resolve HD reference {}: {e}", directory.display()))?;
        if canonical.starts_with(&self.stage) {
            return Err("Existing HD reference points into its own installation stage".into());
        }
        if !self.ancestors.insert(canonical.clone()) {
            return Err(format!(
                "Directory link cycle in existing HD reference: {}",
                directory.display()
            ));
        }
        for entry in fs::read_dir(&canonical).map_err(|e| e.to_string())? {
            self.reporter.check()?;
            let entry = entry.map_err(|e| e.to_string())?;
            self.nodes += 1;
            if self.nodes > MAX_FILES {
                return Err("Existing HD reference contains too many entries".into());
            }
            let name = relative.join(entry.file_name());
            assets::relative_path(&name.to_string_lossy())?;
            if !self.names.insert(name.to_string_lossy().to_lowercase()) {
                return Err("Existing HD reference has filenames that collide on a case-insensitive filesystem".into());
            }
            let source = entry.path().canonicalize().map_err(|e| {
                format!(
                    "Cannot resolve existing HD reference {}: {e}",
                    entry.path().display()
                )
            })?;
            if source.starts_with(&self.stage) {
                return Err("Existing HD reference points into its own installation stage".into());
            }
            let meta = fs::metadata(&source).map_err(|e| e.to_string())?;
            if meta.is_dir() {
                self.visit(&source, &name, depth + 1)?;
            } else if meta.is_file() {
                self.bytes = self
                    .bytes
                    .checked_add(meta.len())
                    .ok_or("HD reference size overflow")?;
                if self.bytes > MAX_BYTES {
                    return Err("Existing HD reference exceeds the 256 GiB limit".into());
                }
                self.files.push(CopyFile {
                    source,
                    relative: name,
                    bytes: meta.len(),
                });
            } else {
                return Err(format!(
                    "Existing HD reference contains a special file: {}",
                    source.display()
                ));
            }
        }
        self.ancestors.remove(&canonical);
        Ok(())
    }
}

fn validated_hd_reference(source: &Path, reporter: &Reporter) -> Result<PathBuf, String> {
    reporter.check()?;
    let status = assets::inspect(&AssetPaths {
        hd_dir: Some(source.to_path_buf()),
        ..Default::default()
    })
    .hd;
    reporter.check()?;
    if !status.ready {
        return Err(format!(
            "Existing HD reference is not usable: {}",
            status.detail
        ));
    }
    status
        .path
        .ok_or("Existing HD reference has no asset root".into())
}

fn copy_reference_tree(
    source: &Path,
    destination: &Path,
    stage: &Path,
    phase: &str,
    reporter: &Reporter,
) -> Result<(), String> {
    reporter.progress(&format!("Scanning {phase}"), 0, None);
    let mut scan = ReferenceInventory {
        reporter,
        files: Vec::new(),
        bytes: 0,
        nodes: 0,
        names: BTreeSet::new(),
        ancestors: BTreeSet::new(),
        stage: stage.canonicalize().map_err(|e| e.to_string())?,
    };
    scan.visit(source, Path::new(""), 0)?;
    scan.files.sort_by(|a, b| a.relative.cmp(&b.relative));
    copy_files(scan.files, destination, phase, reporter)
}

fn copy_stream(
    reader: &mut impl Read,
    target: &Path,
    expected: u64,
    progress: &mut u64,
    total: u64,
    phase: &str,
    reporter: &Reporter,
) -> Result<(), String> {
    reporter.check()?;
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    if fs::symlink_metadata(target).is_ok_and(|m| m.file_type().is_symlink() || !m.is_file()) {
        return Err(format!("Unsafe staging collision: {}", target.display()));
    }
    let mut out = OpenOptions::new()
        .create(true)
        .truncate(true)
        .write(true)
        .open(target)
        .map_err(|e| e.to_string())?;
    let mut buffer = [0u8; 128 * 1024];
    let mut copied = 0u64;
    let mut last = Instant::now();
    loop {
        reporter.check()?;
        let n = reader.read(&mut buffer).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        copied += n as u64;
        if copied > expected {
            return Err("Input changed size or archive exceeded its declared length".into());
        }
        out.write_all(&buffer[..n]).map_err(|e| e.to_string())?;
        *progress += n as u64;
        if last.elapsed() >= Duration::from_millis(80) {
            reporter.progress(phase, *progress, Some(total));
            last = Instant::now();
        }
    }
    if copied != expected {
        return Err(format!(
            "Truncated input while copying {}",
            target.display()
        ));
    }
    Ok(())
}

fn copy_tree(
    source: &Path,
    destination: &Path,
    phase: &str,
    reporter: &Reporter,
) -> Result<(), String> {
    reporter.progress(&format!("Scanning {phase}"), 0, None);
    let files = inventory(source, reporter)?;
    copy_files(files, destination, phase, reporter)
}

fn copy_files(
    files: Vec<CopyFile>,
    destination: &Path,
    phase: &str,
    reporter: &Reporter,
) -> Result<(), String> {
    let total = files.iter().map(|f| f.bytes).sum();
    let mut done = 0;
    fs::create_dir_all(destination).map_err(|e| e.to_string())?;
    for item in files {
        reporter.check()?;
        let meta = fs::symlink_metadata(&item.source).map_err(|e| e.to_string())?;
        if !meta.is_file() || meta.file_type().is_symlink() || meta.len() != item.bytes {
            return Err("Source changed during import".into());
        }
        let mut file = File::open(&item.source).map_err(|e| e.to_string())?;
        copy_stream(
            &mut file,
            &destination.join(item.relative),
            item.bytes,
            &mut done,
            total,
            phase,
            reporter,
        )?;
    }
    reporter.progress(phase, done, Some(total));
    Ok(())
}

fn unzip(source: &Path, destination: &Path, reporter: &Reporter) -> Result<(), String> {
    reporter.progress("Checking ZIP entries", 0, None);
    let mut archive = zip::ZipArchive::new(File::open(source).map_err(|e| e.to_string())?)
        .map_err(|e| format!("Cannot open ZIP: {e}"))?;
    if archive.len() > MAX_FILES {
        return Err("Archive contains too many entries".into());
    }
    let mut names = BTreeMap::new();
    let mut total = 0u64;
    for index in 0..archive.len() {
        reporter.check()?;
        let file = archive.by_index(index).map_err(|e| e.to_string())?;
        let relative = assets::relative_path(file.name())?;
        let mode = file.unix_mode().unwrap_or(0) & 0o170000;
        if mode != 0 && mode != 0o100000 && mode != 0o040000 {
            return Err(format!(
                "Archive links/special entries are not supported: {}",
                file.name()
            ));
        }
        // Archives may omit directory entries. Include every ancestor so case
        // collisions and file/directory conflicts are rejected on every OS.
        for path in relative
            .ancestors()
            .filter(|path| !path.as_os_str().is_empty())
        {
            let directory = path != relative.as_path() || file.is_dir();
            let key = path.to_string_lossy().to_lowercase();
            if let Some((was_dir, spelling)) = names.insert(key, (directory, path.to_path_buf())) {
                if !was_dir || !directory || spelling.as_path() != path {
                    return Err(format!("Duplicate portable ZIP path: {}", file.name()));
                }
            }
        }
        total = total
            .checked_add(file.size())
            .ok_or("Archive size overflow")?;
        if total > MAX_BYTES {
            return Err("Archive exceeds the 256 GiB unpacked limit".into());
        }
    }
    let mut done = 0;
    for index in 0..archive.len() {
        reporter.check()?;
        let mut file = archive.by_index(index).map_err(|e| e.to_string())?;
        let target = destination.join(assets::relative_path(file.name())?);
        if file.is_dir() {
            fs::create_dir_all(target).map_err(|e| e.to_string())?;
        } else {
            let size = file.size();
            copy_stream(
                &mut file,
                &target,
                size,
                &mut done,
                total,
                "Extracting HD ZIP",
                reporter,
            )?;
        }
    }
    reporter.progress("Extracting HD ZIP", done, Some(total));
    Ok(())
}

fn copy_hd_reference(
    source: &Path,
    destination: &Path,
    reporter: &Reporter,
) -> Result<PathBuf, String> {
    let root = validated_hd_reference(source, reporter)?;
    let mut stage = Stage::new(destination)?;
    let payload = stage.directory.join("content");
    copy_reference_tree(
        &root,
        &payload,
        &stage.directory,
        "Copying selected HD reference",
        reporter,
    )?;
    reporter.log(assets::hd_status(&payload)?);
    stage.publish(&payload, destination, reporter)?;
    Ok(destination.to_path_buf())
}

fn install_hd(
    source: &Path,
    destination: &Path,
    overlay: bool,
    reporter: &Reporter,
) -> Result<PathBuf, String> {
    reporter.check()?;
    let source_meta = fs::symlink_metadata(source).map_err(|e| e.to_string())?;
    if source_meta.file_type().is_symlink() {
        return Err("HD import source cannot be a symlink".into());
    }
    let mut stage = Stage::new(destination)?;
    let payload = stage.directory.join("content");
    if source_meta.is_dir() {
        let canonical_source = source.canonicalize().map_err(|e| e.to_string())?;
        let canonical_stage = stage.directory.canonicalize().map_err(|e| e.to_string())?;
        if canonical_stage.starts_with(&canonical_source) {
            return Err("Managed cache cannot be inside the source pack".into());
        }
    }
    // Overlay merges an installed base when one exists; a fresh destination may
    // still install a complete base pack. Final validation rejects add-ons alone.
    if overlay && destination.exists() {
        let base = validated_hd_reference(destination, reporter)?;
        copy_reference_tree(
            &base,
            &payload,
            &stage.directory,
            "Copying existing HD base",
            reporter,
        )?;
    }
    if source_meta.is_dir() {
        let root = assets::normalize_hd(source)?;
        copy_tree(&root, &payload, "Copying selected HD pack", reporter)?;
    } else if source_meta.is_file() {
        let extracted = stage.directory.join("extracted");
        fs::create_dir(&extracted).map_err(|e| e.to_string())?;
        unzip(source, &extracted, reporter)?;
        let root = assets::normalize_hd(&extracted)?;
        if overlay {
            copy_tree(&root, &payload, "Merging HD add-on", reporter)?;
        } else {
            fs::rename(root, &payload).map_err(|e| e.to_string())?;
        }
    } else {
        return Err("Choose an HD folder or ZIP archive".into());
    }
    reporter.progress("Validating HD installation", 0, None);
    reporter.log(assets::hd_status(&payload)?);
    stage.publish(&payload, destination, reporter)?;
    Ok(destination.to_path_buf())
}

#[cfg(windows)]
struct ChildGroup(windows_sys::Win32::Foundation::HANDLE);
#[cfg(windows)]
impl ChildGroup {
    fn new(child: &Child) -> Result<Self, String> {
        use std::os::windows::io::AsRawHandle;
        use windows_sys::Win32::System::JobObjects::*;
        unsafe {
            let handle = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if handle.is_null() {
                return Err(std::io::Error::last_os_error().to_string());
            }
            let group = Self(handle);
            let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                &info as *const _ as *const _,
                std::mem::size_of_val(&info) as u32,
            ) == 0
                || AssignProcessToJobObject(handle, child.as_raw_handle() as _) == 0
            {
                return Err(std::io::Error::last_os_error().to_string());
            }
            Ok(group)
        }
    }
    fn kill(&self) {
        unsafe {
            windows_sys::Win32::System::JobObjects::TerminateJobObject(self.0, 1);
        }
    }
}
#[cfg(windows)]
impl Drop for ChildGroup {
    fn drop(&mut self) {
        unsafe {
            windows_sys::Win32::Foundation::CloseHandle(self.0);
        }
    }
}

#[cfg(unix)]
struct ChildGroup(u32);
#[cfg(unix)]
impl ChildGroup {
    fn new(child: &Child) -> Result<Self, String> {
        Ok(Self(child.id()))
    }
    fn kill(&self) {
        let _ = nix::sys::signal::killpg(
            nix::unistd::Pid::from_raw(self.0 as i32),
            nix::sys::signal::Signal::SIGKILL,
        );
    }
}

fn incomplete_conversion(line: &str) -> bool {
    let fields = line.split_whitespace().collect::<Vec<_>>();
    line.starts_with("FAIL ")
        || (fields.len() == 2
            && fields[1] == "problems"
            && fields[0].parse::<u64>().is_ok_and(|n| n != 0))
        || (fields.last() == Some(&"failed")
            && fields.len() >= 2
            && fields[fields.len() - 2]
                .parse::<u64>()
                .is_ok_and(|n| n != 0))
}

/// Full output of the importer's child processes, kept on disk so a failure can
/// be diagnosed after the launcher's short live log has scrolled or closed.
#[derive(Clone)]
struct ProcessLog {
    path: PathBuf,
    file: Arc<Mutex<File>>,
}

impl ProcessLog {
    /// Starts a fresh log for one import. A log that cannot be created is
    /// reported but never blocks the import.
    fn create(path: &Path, reporter: &Reporter) -> Option<Self> {
        let opened = path
            .parent()
            .map_or(Ok(()), fs::create_dir_all)
            .and_then(|_| File::create(path));
        match opened {
            Ok(file) => Some(Self {
                path: path.to_path_buf(),
                file: Arc::new(Mutex::new(file)),
            }),
            Err(error) => {
                reporter.log(format!(
                    "Cannot write the converter log {}: {error}",
                    path.display()
                ));
                None
            }
        }
    }

    fn line(&self, text: &str) {
        if let Ok(mut file) = self.file.lock() {
            let _ = writeln!(file, "{text}");
        }
    }
}

/// The line that best explains a failed importer: its own `error:` line when it
/// printed one, else the last stderr line that is not progress chatter.
#[derive(Default)]
struct FailureReason {
    explicit: Option<String>,
    last: Option<String>,
}

impl FailureReason {
    fn observe(&mut self, line: &str) {
        let line = line.trim();
        if line.is_empty() {
            return;
        }
        if line.contains(": error: ") || line.starts_with("error: ") {
            self.explicit = Some(line.to_owned());
        } else if !(line.starts_with('[')
            || line.starts_with("Reusing complete model")
            || line.starts_with("Converted ")
            || line.starts_with("mapping: "))
        {
            self.last = Some(line.to_owned());
        }
    }

    fn line(&self) -> Option<&str> {
        self.explicit.as_deref().or(self.last.as_deref())
    }
}

fn read_output(
    mut input: impl Read + Send + 'static,
    reporter: Reporter,
    tail: Arc<Mutex<VecDeque<String>>>,
    incomplete: Arc<AtomicBool>,
    log: Option<ProcessLog>,
    reason: Option<Arc<Mutex<FailureReason>>>,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        let mut buffer = [0u8; 2048];
        let mut line = Vec::new();
        let emit = |bytes: &mut Vec<u8>| {
            let text = String::from_utf8_lossy(bytes)
                .trim_end_matches('\r')
                .to_string();
            if incomplete_conversion(&text) {
                incomplete.store(true, Ordering::Release);
            }
            if let Some(log) = &log {
                log.line(&text);
            }
            if let Some(reason) = &reason {
                if let Ok(mut reason) = reason.lock() {
                    reason.observe(&text);
                }
            }
            reporter.log(&text);
            if let Ok(mut tail) = tail.lock() {
                if tail.len() == LOG_TAIL {
                    tail.pop_front();
                }
                tail.push_back(text);
            }
            bytes.clear();
        };
        loop {
            match input.read(&mut buffer) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    for byte in &buffer[..n] {
                        if *byte == b'\n' {
                            emit(&mut line);
                        } else if line.len() < MAX_LINE {
                            line.push(*byte);
                        }
                    }
                }
            }
        }
        if !line.is_empty() {
            emit(&mut line);
        }
    })
}

/// NTSTATUS values a child exits with when Windows itself stopped it. A player
/// sees these as large hex exit codes, so name the usual cause.
fn windows_status_meaning(code: u32) -> Option<&'static str> {
    Some(match code {
        0xC000_0135 => "a required DLL was not found; install the Microsoft Visual C++ 2015-2022 x64 Redistributable",
        0xC000_007B => "a DLL is for the wrong architecture or damaged; reinstall the Microsoft Visual C++ 2015-2022 x64 Redistributable",
        0xC000_0139 | 0xC000_0142 => "a required DLL failed to load",
        0xC000_0005 => "it crashed with an access violation",
        0xC000_00FD => "it crashed with a stack overflow",
        0xC000_0409 => "it aborted on an internal check",
        0xC000_0017 | 0xC000_009A => "the system ran out of memory",
        0xC000_0022 => "access was denied; antivirus may be blocking it",
        _ => return None,
    })
}

fn describe_exit(program: &Path, status: std::process::ExitStatus) -> String {
    let name = program.file_name().map_or_else(
        || program.display().to_string(),
        |n| n.to_string_lossy().into_owned(),
    );
    match status
        .code()
        .and_then(|code| windows_status_meaning(code as u32))
    {
        Some(meaning) => format!("{name} exited with {status} ({meaning})"),
        None => format!("{name} exited with {status}"),
    }
}

/// Failure text for the launcher notice: the reason first, then where the full
/// output is. Without a log file, the last output lines stand in for it.
fn failure_message(
    exit: &str,
    reason: Option<&str>,
    log: Option<&Path>,
    tail: &[String],
) -> String {
    let mut message = match reason {
        Some(reason) => format!("{reason}\n{exit}."),
        None => format!("{exit} without an error message."),
    };
    match log {
        Some(log) => message.push_str(&format!(" Full log: {}", log.display())),
        None => {
            let start = tail.len().saturating_sub(8);
            for line in &tail[start..] {
                message.push('\n');
                message.push_str(line);
            }
        }
    }
    message
}

fn run(
    program: &Path,
    args: &[OsString],
    cwd: &Path,
    environment: &[(OsString, OsString)],
    reporter: &Reporter,
    log: Option<&ProcessLog>,
) -> Result<Vec<String>, String> {
    reporter.check()?;
    let command_line = format!(
        "Running {} {}",
        program.display(),
        args.iter()
            .map(|a| a.to_string_lossy())
            .collect::<Vec<_>>()
            .join(" ")
    );
    if let Some(log) = log {
        log.line(&format!(
            "\n{command_line}\n(working folder {})",
            cwd.display()
        ));
    }
    reporter.log(command_line);
    let mut command = Command::new(program);
    command
        .args(args)
        .current_dir(cwd)
        .envs(environment.iter().cloned())
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(error) => {
            let message = format!(
                "Cannot start {}: {error}. Re-extract the full package; if antivirus quarantined it, restore it or allow it.",
                program.display()
            );
            if let Some(log) = log {
                log.line(&message);
            }
            return Err(message);
        }
    };
    let group = match ChildGroup::new(&child) {
        Ok(group) => group,
        Err(error) => {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!(
                "Cannot establish child cancellation group: {error}"
            ));
        }
    };
    let tail = Arc::new(Mutex::new(VecDeque::new()));
    let incomplete = Arc::new(AtomicBool::new(false));
    let reason = Arc::new(Mutex::new(FailureReason::default()));
    let output = read_output(
        child.stdout.take().unwrap(),
        reporter.clone(),
        tail.clone(),
        incomplete.clone(),
        log.cloned(),
        None,
    );
    let errors = read_output(
        child.stderr.take().unwrap(),
        reporter.clone(),
        tail.clone(),
        incomplete.clone(),
        log.cloned(),
        Some(reason.clone()),
    );
    let result = loop {
        if reporter.check().is_err() {
            group.kill();
            let _ = child.kill();
            let _ = child.wait();
            break Err("Cancelled; importer and helper processes stopped".to_string());
        }
        match child.try_wait() {
            Ok(Some(status)) => {
                break if status.success() {
                    Ok(())
                } else {
                    Err(describe_exit(program, status))
                }
            }
            Ok(None) => thread::sleep(Duration::from_millis(50)),
            Err(error) => {
                group.kill();
                let _ = child.kill();
                let _ = child.wait();
                break Err(error.to_string());
            }
        }
    };
    // Also stop an orphan helper that survived an early parent failure, so pipe
    // reader joins cannot leave the installer waiting on a compiler grandchild.
    group.kill();
    let _ = output.join();
    let _ = errors.join();
    let lines = tail
        .lock()
        .map_err(|e| e.to_string())?
        .iter()
        .cloned()
        .collect::<Vec<_>>();
    if let Some(log) = log {
        log.line(&match &result {
            Ok(()) => "Finished successfully".to_string(),
            Err(error) => error.clone(),
        });
    }
    if let Err(exit) = result {
        if reporter.check().is_err() {
            return Err(exit);
        }
        let reason = reason.lock().ok();
        return Err(failure_message(
            &exit,
            reason.as_ref().and_then(|reason| reason.line()),
            log.map(|log| log.path.as_path()),
            &lines,
        ));
    }
    if incomplete.load(Ordering::Acquire) {
        return Err("Importer reported failed conversions or unresolved mapping problems; previous installation preserved".into());
    }
    Ok(lines)
}

fn copy_existing_converted(
    source: &Path,
    destination: &Path,
    reporter: &Reporter,
) -> Result<(), String> {
    reporter.progress("Checking existing converted cache", 0, None);
    match inventory(source, reporter) {
        Ok(files) => copy_files(
            files,
            destination,
            "Copying existing converted models",
            reporter,
        ),
        Err(error)
            if error.starts_with("Unsafe asset path:")
                || error.starts_with("Reserved Windows asset path:")
                || error
                    == "Folder has filenames that collide on a case-insensitive filesystem" =>
        {
            reporter.log(format!("Rebuilding legacy converted cache with portable filenames; keeping the original until the full import succeeds: {error}"));
            Ok(())
        }
        Err(error) => Err(error),
    }
}

/// Skip an import whose record still matches the install, importer and the
/// published files; otherwise import and record the result. Per-asset reuse
/// inside a re-import is the importer's job (existing GLBs, recipe hashes).
fn import_d2r_cached(
    request: &D2rImportRequest,
    reporter: &Reporter,
) -> Result<(PathBuf, bool), String> {
    reporter.check()?;
    let destination = request.cache_dir.join("glb");
    let stamp_path = request.cache_dir.join(d2r_cache::ART_STAMP);
    reporter.progress("Checking previous D2R import", 0, None);
    let source = assets::is_d2r_install(&request.game_dir)
        .then(|| {
            d2r_cache::importer_fingerprint(&request.importer_dir)
                .and_then(|hash| d2r_cache::source_fingerprint(&request.game_dir, hash))
        })
        .and_then(Result::ok);
    if let Some(source) = &source {
        let stamp = d2r_cache::load_stamp(&stamp_path);
        let actual = d2r_cache::scan_outputs(&destination, &[]);
        let decision = d2r_cache::decide(
            stamp.as_ref(),
            source,
            actual.as_ref().map_err(String::as_str),
        );
        match decision {
            CacheDecision::Skip
                if assets::inspect(&AssetPaths {
                    d2r_dir: Some(destination.clone()),
                    ..Default::default()
                })
                .d2r_ready() =>
            {
                reporter.log(format!(
                    "D2R art from build {} is already imported and unchanged; nothing to convert.",
                    source.build
                ));
                return Ok((destination, true));
            }
            CacheDecision::Skip => {
                reporter.log("Previous import fails validation; importing again")
            }
            CacheDecision::Import(reason) => reporter.log(format!("Importing D2R art: {reason}")),
        }
    }
    let published = import_d2r(request, reporter)?;
    if let Some(source) = source {
        let recorded = d2r_cache::scan_outputs(&published, &[]).and_then(|outputs| {
            d2r_cache::write_stamp(&stamp_path, &ImportStamp::new(source, outputs))
        });
        if let Err(error) = recorded {
            reporter.log(format!(
                "Import installed; its cache record could not be written: {error}"
            ));
        }
    }
    Ok((published, false))
}

fn import_d2r(request: &D2rImportRequest, reporter: &Reporter) -> Result<PathBuf, String> {
    let name = if cfg!(windows) {
        "d2r-convert.exe"
    } else {
        "d2r-convert"
    };
    let converter = std::env::var_os("DAMNATIONX_D2R_RUST_CONVERTER")
        .map(PathBuf::from)
        .unwrap_or_else(|| request.importer_dir.join("bin").join(name));
    import_d2r_with_converter(request, reporter, Some(converter))
}

fn import_d2r_with_converter(
    request: &D2rImportRequest,
    reporter: &Reporter,
    converter: Option<PathBuf>,
) -> Result<PathBuf, String> {
    reporter.check()?;
    if !assets::is_d2r_install(&request.game_dir) {
        return Err(D2R_INSTALL_REQUIRED.into());
    }
    let has_recipes = request.importer_dir.join("mapping.toml").is_file()
        && request.importer_dir.join("env_mapping.toml").is_file();
    if !has_recipes {
        return Err("Importer recipes are missing; choose resources/d2r-import or the repository's tools/d2r-import".into());
    }
    let log = request
        .log_file
        .as_deref()
        .and_then(|path| ProcessLog::create(path, reporter));
    if let Some(log) = &log {
        log.line(&format!(
            "DamnationX launcher {} on {} {}\nD2R install: {}\nCache: {}\nImporter: {}\nConverter: {}",
            env!("CARGO_PKG_VERSION"),
            std::env::consts::OS,
            std::env::consts::ARCH,
            request.game_dir.display(),
            request.cache_dir.display(),
            request.importer_dir.display(),
            converter
                .as_ref()
                .map_or_else(|| "unavailable".into(), |c| c.display().to_string()),
        ));
        reporter.log(format!("Converter log: {}", log.path.display()));
    }
    let result = convert_d2r(request, reporter, converter, log.as_ref());
    if let (Err(error), Some(log)) = (&result, &log) {
        log.line(&format!("\nImport failed: {error}"));
    }
    match (result, &log) {
        (Err(error), Some(log)) if !error.contains("Full log: ") && reporter.check().is_ok() => {
            Err(format!("{error}\nFull log: {}", log.path.display()))
        }
        (result, _) => result,
    }
}

/// On Windows the converter needs the Visual C++ runtime DLLs that ship beside
/// the launcher, not beside the converter in resources/d2r-import/bin. Without
/// the system-wide redistributable it could not start (0xC0000135), so put the
/// launcher folder first on the child's DLL search path.
fn converter_environment() -> Vec<(OsString, OsString)> {
    if !cfg!(windows) {
        return Vec::new();
    }
    let Some(folder) = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
    else {
        return Vec::new();
    };
    let existing = std::env::var_os("PATH").unwrap_or_default();
    let paths = std::iter::once(folder).chain(std::env::split_paths(&existing));
    std::env::join_paths(paths)
        .map(|joined| vec![(OsString::from("PATH"), joined)])
        .unwrap_or_default()
}

fn convert_d2r(
    request: &D2rImportRequest,
    reporter: &Reporter,
    converter: Option<PathBuf>,
    log: Option<&ProcessLog>,
) -> Result<PathBuf, String> {
    let destination = request.cache_dir.join("glb");
    let mut stage = Stage::new(&destination)?;
    let mut work = crate::d2r_resume::Work::open(&request.cache_dir, &request.game_dir)?;
    let cache = work.cache.canonicalize().map_err(|e| {
        format!(
            "Cannot open the conversion cache {}: {e}",
            work.cache.display()
        )
    })?;
    let game = request.game_dir.canonicalize().map_err(|e| {
        format!(
            "Cannot open the D2R folder {}: {e}",
            request.game_dir.display()
        )
    })?;
    let importer = request.importer_dir.canonicalize().map_err(|e| {
        format!(
            "Cannot open the importer folder {}: {e}",
            request.importer_dir.display()
        )
    })?;
    reporter.log(format!(
        "Conversion work is retained at {} for retries after failure or cancellation",
        cache.display()
    ));
    if !cache.join("glb").exists() {
        if work.continuing && destination.is_dir() {
            // An interrupted seed copy must not look like damaged conversion
            // work on the next retry. Publish the seed only after copying it.
            let seed = stage.directory.join("seed");
            copy_existing_converted(&destination, &seed, reporter)?;
            if seed.is_dir() {
                fs::rename(seed, cache.join("glb")).map_err(|e| e.to_string())?;
            }
        }
        fs::create_dir_all(cache.join("glb")).map_err(|e| e.to_string())?;
    }
    let tool_name = if cfg!(windows) {
        "d2rtool.exe"
    } else {
        "d2rtool"
    };
    let packaged_tool = request.importer_dir.join("bin").join(tool_name);
    let cached_tool = request.cache_dir.join("bin").join(tool_name);
    if let Some(converter) = converter {
        let helper = if packaged_tool.is_file() {
            &packaged_tool
        } else {
            &cached_tool
        };
        if !converter.is_file() || !helper.is_file() {
            return Err("D2R importer is incomplete: the converter or archive helper is missing from the release".into());
        }
        let converter = converter.canonicalize().map_err(|e| e.to_string())?;
        let helper = helper.canonicalize().map_err(|e| e.to_string())?;
        let steps = [
            (
                "models",
                "Converting mapped models",
                "mapping-all",
                Some("mapping.toml"),
            ),
            (
                "environment",
                "Building level environments",
                "environment",
                Some("env_mapping.toml"),
            ),
            ("ui", "Extracting D2R interface", "ui", None),
            ("portal", "Extracting Town Portal fields", "portal", None),
        ];
        for (index, (phase, label, command, recipe)) in steps.iter().enumerate() {
            reporter.check()?;
            let status = assets::inspect(&AssetPaths {
                d2r_dir: Some(cache.join("glb")),
                ..Default::default()
            });
            let ready = match *phase {
                "models" => status.d2r_mapping.ready,
                "environment" => status.d2r_environment.ready,
                "ui" => status.d2r_ui.ready,
                "portal" => status.d2r_portal.ready,
                _ => false,
            };
            if work.begin(phase, &request.importer_dir, ready)? {
                reporter.log(format!("Reusing completed phase: {label}"));
                reporter.progress(label, (index + 1) as u64, Some(steps.len() as u64));
                continue;
            }
            // This is the retained work cache, never the published destination.
            // The converter refuses to overwrite a completed mapping manifest.
            if *phase == "models" {
                let manifest = cache.join("glb/mapping.resolved.json");
                if manifest.exists() {
                    fs::remove_file(manifest).map_err(|e| e.to_string())?;
                }
            }
            let mut args = vec![
                OsString::from(command),
                game.as_os_str().into(),
                cache.as_os_str().into(),
                helper.as_os_str().into(),
            ];
            if let Some(recipe) = recipe {
                args.push(importer.join(recipe).into_os_string());
            }
            reporter.progress(label, index as u64, Some(steps.len() as u64));
            run(
                &converter,
                &args,
                &importer,
                &converter_environment(),
                reporter,
                log,
            )?;
            let status = assets::inspect(&AssetPaths {
                d2r_dir: Some(cache.join("glb")),
                ..Default::default()
            });
            let valid = match *phase {
                "models" => status.d2r_mapping.ready,
                "environment" => status.d2r_environment.ready,
                "ui" => status.d2r_ui.ready,
                "portal" => status.d2r_portal.ready,
                _ => false,
            };
            if !valid {
                return Err(format!("Rust importer produced incomplete {phase} assets; previous installation retained"));
            }
            work.complete(phase)?;
            reporter.progress(label, (index + 1) as u64, Some(steps.len() as u64));
        }
        stage.publish(&cache.join("glb"), &destination, reporter)?;
        return Ok(destination);
    }
    Err("Rust D2R converter is unavailable".into())
}

fn publish_sound_packs(
    stage: &mut Stage,
    payload: &Path,
    destination: &Path,
    reporter: &Reporter,
) -> Result<(), String> {
    reporter.check()?;
    fs::create_dir_all(destination).map_err(|e| e.to_string())?;
    let previous = stage.directory.join("previous-sounds");
    fs::create_dir(&previous).map_err(|e| e.to_string())?;
    let mut backed_up = Vec::new();
    let mut published = Vec::new();
    // Each directory replacement is atomic. The existing install lock covers
    // the two-pack commit; unrelated mods and the user's saves never move.
    let commit = (|| -> Result<(), String> {
        for name in assets::D2R_SOUND_PACKS {
            let target = destination.join(name);
            if let Ok(meta) = fs::symlink_metadata(&target) {
                if meta.file_type().is_symlink() || !meta.is_dir() {
                    return Err(format!(
                        "Existing sound pack is a link or special file: {}",
                        target.display()
                    ));
                }
                fs::rename(&target, previous.join(name)).map_err(|e| e.to_string())?;
                backed_up.push(name);
            }
        }
        // Cancellation stays outside this short commit region: complete both
        // validated packs, or restore every directory moved above.
        for name in assets::D2R_SOUND_PACKS {
            fs::rename(payload.join(name), destination.join(name)).map_err(|e| e.to_string())?;
            published.push(name);
        }
        Ok(())
    })();
    if let Err(error) = commit {
        let mut restore_errors = Vec::new();
        for name in published.into_iter().rev() {
            if let Err(e) = fs::remove_dir_all(destination.join(name)) {
                restore_errors.push(e.to_string());
            }
        }
        for name in backed_up.into_iter().rev() {
            if let Err(e) = fs::rename(previous.join(name), destination.join(name)) {
                restore_errors.push(e.to_string());
            }
        }
        if !restore_errors.is_empty() {
            stage.preserve = true;
            return Err(format!("Sound publication failed ({error}); restore needs attention ({}). Previous packs remain at {}", restore_errors.join("; "), previous.display()));
        }
        return Err(format!(
            "Sound publication failed; previous packs restored: {error}"
        ));
    }
    reporter.log(format!(
        "Installed D2R classic and remastered sounds in {}",
        destination.display()
    ));
    Ok(())
}

fn import_d2r_sounds_cached(
    request: &D2rSoundImportRequest,
    reporter: &Reporter,
) -> Result<(PathBuf, bool), String> {
    reporter.check()?;
    let Some(stamp_path) = request.stamp.clone() else {
        return import_d2r_sounds(request, reporter).map(|path| (path, false));
    };
    let source = d2r_cache::program_fingerprint(&request.program, request.classic_runtime)
        .and_then(|hash| d2r_cache::source_fingerprint(&request.game_dir, hash))
        .ok();
    if let Some(source) = &source {
        let stamp = d2r_cache::load_stamp(&stamp_path);
        let actual = d2r_cache::scan_outputs(&request.destination, &assets::D2R_SOUND_PACKS);
        match d2r_cache::decide(
            stamp.as_ref(),
            source,
            actual.as_ref().map_err(String::as_str),
        ) {
            CacheDecision::Skip
                if assets::D2R_SOUND_PACKS.iter().all(|name| {
                    assets::sound_pack_status(&request.destination.join(name)).is_ok()
                }) =>
            {
                reporter.log(format!(
                    "D2R sounds from build {} are already imported and unchanged.",
                    source.build
                ));
                return Ok((request.destination.clone(), true));
            }
            CacheDecision::Skip => {
                reporter.log("Previous sound import fails validation; importing again")
            }
            CacheDecision::Import(reason) => {
                reporter.log(format!("Importing D2R sounds: {reason}"))
            }
        }
    }
    let published = import_d2r_sounds(request, reporter)?;
    if let Some(source) = source {
        let recorded =
            d2r_cache::scan_outputs(&published, &assets::D2R_SOUND_PACKS).and_then(|outputs| {
                d2r_cache::write_stamp(&stamp_path, &ImportStamp::new(source, outputs))
            });
        if let Err(error) = recorded {
            reporter.log(format!(
                "Sounds installed; their cache record could not be written: {error}"
            ));
        }
    }
    Ok((published, false))
}

fn import_d2r_sounds(
    request: &D2rSoundImportRequest,
    reporter: &Reporter,
) -> Result<PathBuf, String> {
    reporter.check()?;
    let readable = match assets::d2r_layout(&request.game_dir) {
        Some(assets::D2rLayout::BattleNet { data }) => data.join("indices").is_dir(),
        Some(assets::D2rLayout::Steam { .. }) => true,
        None => false,
    };
    if !readable {
        return Err(D2R_INSTALL_REQUIRED.into());
    }
    let program = request
        .program
        .canonicalize()
        .map_err(|e| format!("Select a game executable with D2R sound import support: {e}"))?;
    let source = request.game_dir.canonicalize().map_err(|e| e.to_string())?;
    let mut stage = Stage::new(&request.destination)?;
    if stage
        .directory
        .canonicalize()
        .map_err(|e| e.to_string())?
        .starts_with(&source)
    {
        return Err("Sound output cannot be inside the owned D2R installation".into());
    }
    let payload = stage.directory.join("sound-packs");
    fs::create_dir(&payload).map_err(|e| e.to_string())?;
    let mut args: Vec<OsString> = vec!["--import-d2-sounds".into(), "d2r".into()];
    let result_root;
    if request.classic_runtime {
        let scratch = stage.directory.join("native-import");
        fs::create_dir(&scratch).map_err(|e| e.to_string())?;
        args.extend([
            "--save-dir".into(),
            scratch.clone().into_os_string(),
            "--config-dir".into(),
            scratch.clone().into_os_string(),
            "--set".into(),
            format!("Audio.Diablo II Resurrected Path={}", source.display()).into(),
        ]);
        if let Some(data) = &request.data_dir {
            args.extend(["--data-dir".into(), data.as_os_str().to_owned()]);
        }
        result_root = scratch.join("mods");
    } else {
        args.extend([
            "--d2r-game-dir".into(),
            source.into_os_string(),
            "--sound-output-dir".into(),
            payload.clone().into_os_string(),
        ]);
        result_root = payload;
    }
    reporter.progress("Importing owned D2R classic and remastered sounds", 0, None);
    run(
        &program,
        &args,
        program.parent().unwrap(),
        &[],
        reporter,
        None,
    )?;
    for (index, name) in assets::D2R_SOUND_PACKS.iter().enumerate() {
        reporter.check()?;
        reporter.progress("Validating imported sound packs", index as u64, Some(2));
        reporter.log(format!(
            "{name}: {}",
            assets::sound_pack_status(&result_root.join(name))?
        ));
    }
    reporter.progress("Publishing classic and remastered sounds", 2, Some(2));
    publish_sound_packs(&mut stage, &result_root, &request.destination, reporter)?;
    Ok(request.destination.clone())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn reporter() -> Reporter {
        let (sender, receiver) = mpsc::sync_channel(128);
        // A detached receiver is intentional: progress loss never blocks copies.
        drop(receiver);
        Reporter {
            sender,
            cancel: Arc::new(AtomicBool::new(false)),
            dropped: Arc::new(AtomicU64::new(0)),
        }
    }

    fn sound_pack(path: &Path, marker: &[u8]) {
        fs::create_dir_all(path.join("sfx")).unwrap();
        fs::write(
            path.join("manifest.ini"),
            "[mod]\nname=source-free fixture\n",
        )
        .unwrap();
        let mut wav = vec![0u8; 44];
        wav[..4].copy_from_slice(b"RIFF");
        wav[8..12].copy_from_slice(b"WAVE");
        wav.extend_from_slice(marker);
        fs::write(path.join("sfx/test.wav"), wav).unwrap();
    }

    fn tree_bytes(path: &Path) -> BTreeMap<PathBuf, Vec<u8>> {
        inventory(path, &reporter())
            .unwrap()
            .into_iter()
            .map(|f| (f.relative, fs::read(f.source).unwrap()))
            .collect()
    }

    #[cfg(unix)]
    fn sound_worker_fixture(root: &Path) -> D2rSoundImportRequest {
        use std::os::unix::fs::PermissionsExt;
        let game = root.join("owned game with spaces");
        fs::create_dir_all(game.join("Data/indices")).unwrap();
        fs::write(game.join(".build.info"), "source-free install marker").unwrap();
        let program = root.join("native sound importer with spaces");
        fs::write(
            &program,
            r#"#!/bin/sh
while [ "$#" -gt 0 ]; do
  case "$1" in
    --import-d2-sounds) mode="$2"; shift 2;;
    --d2r-game-dir) source="$2"; shift 2;;
    --sound-output-dir) out="$2"; shift 2;;
    *) exit 2;;
  esac
done
printf %s "$mode" > "$source/called"
if [ -f "$source/pause" ]; then echo 'waiting in native sound import'; sleep 30; fi
if [ -f "$source/fail-native" ]; then echo 'owned archive could not be opened'; exit 7; fi
for name in d2r-sounds-classic d2r-sounds-advanced; do
  if [ "$name" = d2r-sounds-advanced ] && [ -f "$source/fail-validation" ]; then break; fi
  mkdir -p "$out/$name/sfx"
  printf 'RIFF0000WAVE0000000000000000000000000000000000000000' > "$out/$name/sfx/test.wav"
  printf '[mod]\nname=source-free test sound pack\n' > "$out/$name/manifest.ini"
done
echo 'Imported 1 classic and 1 remastered sound'
"#,
        )
        .unwrap();
        fs::set_permissions(&program, fs::Permissions::from_mode(0o755)).unwrap();
        let destination = root.join("native user preferences/mods");
        for name in assets::D2R_SOUND_PACKS {
            sound_pack(&destination.join(name), b"previous sound bytes");
        }
        fs::create_dir_all(destination.join("unrelated-user-mod")).unwrap();
        fs::write(
            destination.join("unrelated-user-mod/keep.txt"),
            b"untouched",
        )
        .unwrap();
        D2rSoundImportRequest {
            program,
            game_dir: game,
            destination,
            classic_runtime: false,
            data_dir: None,
            stamp: None,
        }
    }

    #[test]
    #[cfg(unix)]
    fn repeated_sound_import_is_skipped_until_outputs_or_install_change() {
        let temp = tempfile::tempdir().unwrap();
        let mut request = sound_worker_fixture(temp.path());
        let stamp = temp.path().join("d2r-models").join(d2r_cache::SOUND_STAMP);
        request.stamp = Some(stamp.clone());
        let called = request.game_dir.join("called");
        // First import runs the converter and records the result.
        assert!(!import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        assert!(called.is_file() && stamp.is_file());
        // Unchanged: the converter is not started again.
        fs::remove_file(&called).unwrap();
        assert!(import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        assert!(!called.exists());
        // A missing output re-imports.
        fs::remove_file(request.destination.join("d2r-sounds-classic/sfx/test.wav")).unwrap();
        assert!(!import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        assert!(called.is_file());
        fs::remove_file(&called).unwrap();
        assert!(import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        // A patched install re-imports.
        fs::write(request.game_dir.join(".build.info"), "patched").unwrap();
        assert!(!import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        assert!(called.is_file());
        // Unrelated mods are never part of the record.
        assert_eq!(
            fs::read(request.destination.join("unrelated-user-mod/keep.txt")).unwrap(),
            b"untouched"
        );
    }

    /// Rewrites a fixture install into Steam's shape: no `.build.info` or
    /// `Data/indices`, and a static store described by `Data/.build.config`.
    fn make_steam_install(game: &Path) {
        let _ = fs::remove_file(game.join(".build.info"));
        let _ = fs::remove_dir_all(game.join("Data/indices"));
        fs::create_dir_all(game.join("Data")).unwrap();
        fs::write(
            game.join("Data/.build.config"),
            "root = bf9064519cc56ba5fe197ce27e4729a1\n",
        )
        .unwrap();
        fs::write(game.join("Data/00-00000001.data"), b"").unwrap();
    }

    #[test]
    #[cfg(unix)]
    fn steam_install_sounds_import_and_reimport_after_a_patch() {
        let temp = tempfile::tempdir().unwrap();
        let mut request = sound_worker_fixture(temp.path());
        make_steam_install(&request.game_dir);
        request.stamp = Some(temp.path().join("d2r-models").join(d2r_cache::SOUND_STAMP));
        let called = request.game_dir.join("called");
        assert!(!import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        assert_eq!(fs::read_to_string(&called).unwrap(), "d2r");
        for name in assets::D2R_SOUND_PACKS {
            assert!(assets::sound_pack_status(&request.destination.join(name)).is_ok());
        }
        fs::remove_file(&called).unwrap();
        assert!(import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        assert!(!called.exists());
        // A Steam patch rewrites the build config.
        fs::write(request.game_dir.join("Data/.build.config"), "root = 01\n").unwrap();
        assert!(!import_d2r_sounds_cached(&request, &reporter()).unwrap().1);
        assert!(called.is_file());
        // Without its build config the folder is not an install, and nothing runs.
        fs::remove_file(request.game_dir.join("Data/.build.config")).unwrap();
        fs::remove_file(&called).unwrap();
        let error = import_d2r_sounds(&request, &reporter()).unwrap_err();
        assert_eq!(error, D2R_INSTALL_REQUIRED);
        assert!(!called.exists());
    }

    #[test]
    fn steam_install_passes_the_art_import_gate() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("Diablo II Resurrected");
        make_steam_install(&game);
        let request = D2rImportRequest {
            game_dir: game,
            cache_dir: temp.path().join("cache"),
            importer_dir: temp.path().join("no recipes"),
            log_file: None,
        };
        // The install is accepted; the request stops at the missing recipes instead.
        let error = import_d2r_with_converter(&request, &reporter(), None).unwrap_err();
        assert!(error.contains("recipes are missing"), "{error}");
    }

    #[test]
    #[cfg(unix)]
    fn sound_worker_publishes_both_packs_and_preserves_them_on_native_or_validation_failure() {
        let temp = tempfile::tempdir().unwrap();
        let request = sound_worker_fixture(temp.path());
        import_d2r_sounds(&request, &reporter()).unwrap();
        for name in assets::D2R_SOUND_PACKS {
            assert!(assets::sound_pack_status(&request.destination.join(name)).is_ok());
        }
        assert_eq!(
            fs::read(request.destination.join("unrelated-user-mod/keep.txt")).unwrap(),
            b"untouched"
        );
        assert_eq!(
            fs::read_to_string(request.game_dir.join("called")).unwrap(),
            "d2r"
        );
        let accepted = tree_bytes(&request.destination);
        for marker in ["fail-native", "fail-validation"] {
            fs::write(request.game_dir.join(marker), b"fail this import").unwrap();
            assert!(import_d2r_sounds(&request, &reporter()).is_err());
            assert_eq!(tree_bytes(&request.destination), accepted);
            fs::remove_file(request.game_dir.join(marker)).unwrap();
        }
        fs::remove_dir(request.game_dir.join("Data/indices")).unwrap();
        fs::remove_file(request.game_dir.join("called")).unwrap();
        assert!(import_d2r_sounds(&request, &reporter())
            .unwrap_err()
            .contains("Data/indices"));
        assert!(!request.game_dir.join("called").exists());
        assert_eq!(tree_bytes(&request.destination), accepted);
    }

    #[test]
    fn sound_pair_commit_rolls_back_after_a_second_pack_publish_failure() {
        let temp = tempfile::tempdir().unwrap();
        let destination = temp.path().join("mods");
        for name in assets::D2R_SOUND_PACKS {
            sound_pack(&destination.join(name), b"previous");
        }
        let original = tree_bytes(&destination);
        let mut stage = Stage::new(&destination).unwrap();
        assert!(
            Stage::new(&destination).is_err(),
            "concurrent installers must not share the destination"
        );
        let payload = stage.directory.join("payload");
        sound_pack(&payload.join(assets::D2R_SOUND_PACKS[0]), b"new classic");
        // Simulate a filesystem failure after validation: the second pack is
        // missing while the first is already atomically renamed into place.
        assert!(
            publish_sound_packs(&mut stage, &payload, &destination, &reporter())
                .unwrap_err()
                .contains("previous packs restored")
        );
        assert_eq!(tree_bytes(&destination), original);
    }

    #[test]
    #[cfg(unix)]
    fn cancelling_native_sound_import_preserves_both_working_packs() {
        let temp = tempfile::tempdir().unwrap();
        let request = sound_worker_fixture(temp.path());
        let original = tree_bytes(&request.destination);
        fs::write(request.game_dir.join("pause"), b"wait for cancellation").unwrap();
        let job = JobHandle::spawn(JobRequest::ImportD2rSounds(request.clone()));
        let deadline = Instant::now() + Duration::from_secs(5);
        while !request.game_dir.join("called").exists() && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(10));
        }
        assert!(
            request.game_dir.join("called").exists(),
            "native test importer never started"
        );
        job.cancel();
        let result = loop {
            if let Some(result) = job.poll().into_iter().find_map(|event| match event {
                JobEvent::Finished(result) => Some(result),
                _ => None,
            }) {
                break result;
            }
            assert!(Instant::now() < deadline, "cancelled importer did not stop");
            thread::sleep(Duration::from_millis(10));
        };
        assert!(result.unwrap_err().contains("Cancelled"));
        assert_eq!(tree_bytes(&request.destination), original);
    }
    fn base(path: &Path, marker: &[u8]) {
        fs::create_dir_all(path.join("sprites")).unwrap();
        fs::write(
            path.join("sprites.txt"),
            "clip hero sprites/hero.png 1 1 1 1 0 0 1\n",
        )
        .unwrap();
        fs::write(path.join("sprites/hero.png"), marker).unwrap();
    }
    fn zip_file(path: &Path, entries: &[(&str, &[u8])]) {
        let mut zip = zip::ZipWriter::new(File::create(path).unwrap());
        for (name, data) in entries {
            zip.start_file(*name, zip::write::SimpleFileOptions::default())
                .unwrap();
            zip.write_all(data).unwrap();
        }
        zip.finish().unwrap();
    }
    #[test]
    fn malicious_archive_and_failure_preserve_existing_installation() {
        let temp = tempfile::tempdir().unwrap();
        let installed = temp.path().join("installed");
        base(&installed, b"original");
        let archive = temp.path().join("bad.zip");
        zip_file(
            &archive,
            &[("../outside", b"bad"), ("hd/sprites.txt", b"bad")],
        );
        assert!(install_hd(&archive, &installed, false, &reporter()).is_err());
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"original"
        );
        assert!(!temp.path().join("outside").exists());
        let invalid = temp.path().join("invalid");
        fs::create_dir(&invalid).unwrap();
        assert!(install_hd(&invalid, &installed, false, &reporter()).is_err());
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"original"
        );
    }
    #[test]
    fn cancellation_before_publish_preserves_previous_tree() {
        let temp = tempfile::tempdir().unwrap();
        let installed = temp.path().join("installed");
        base(&installed, b"original");
        let mut stage = Stage::new(&installed).unwrap();
        let payload = stage.directory.join("content");
        base(&payload, b"replacement");
        let reporter = reporter();
        reporter.cancel.store(true, Ordering::Release);
        assert!(stage.publish(&payload, &installed, &reporter).is_err());
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"original"
        );
    }
    #[test]
    fn failed_commit_restores_previous_directory() {
        let temp = tempfile::tempdir().unwrap();
        let installed = temp.path().join("installed");
        base(&installed, b"original");
        let mut stage = Stage::new(&installed).unwrap();
        let absent = stage.directory.join("missing-payload");
        assert!(stage.publish(&absent, &installed, &reporter()).is_err());
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"original"
        );
    }
    #[test]
    fn stage_drop_releases_its_lock_while_a_duplicate_descriptor_stays_open() {
        let temp = tempfile::tempdir().unwrap();
        let first = temp.path().join("first");
        let second = temp.path().join("second");
        let stage = Stage::new(&first).unwrap();
        let duplicate = stage._lock.try_clone().unwrap();
        assert!(
            Stage::new(&second).is_err(),
            "a live install must retain exclusive ownership"
        );
        drop(stage);
        let next = Stage::new(&second).expect(
            "dropping the owner must release its lock even while a duplicate handle remains open",
        );
        assert!(duplicate.metadata().unwrap().is_file());
        drop(next);
        drop(duplicate);
    }
    #[test]
    fn three_d_overlay_keeps_base_images() {
        let temp = tempfile::tempdir().unwrap();
        let installed = temp.path().join("installed");
        base(&installed, b"original");
        let addon = temp.path().join("addon/hd/3d/free");
        fs::create_dir_all(&addon).unwrap();
        fs::write(addon.join("manifest.json"), r#"{"models":[]}"#).unwrap();
        fs::write(addon.join("extra.txt"), "3D overlay").unwrap();
        install_hd(&temp.path().join("addon"), &installed, true, &reporter()).unwrap();
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"original"
        );
        assert!(installed.join("3d/free/extra.txt").is_file());
    }
    #[cfg(unix)]
    #[test]
    fn overlay_migrates_a_legacy_linked_baseline_into_regular_staged_files() {
        let temp = tempfile::tempdir().unwrap();
        let owned = temp.path().join("owned");
        base(&owned, b"owned base");
        let installed = temp.path().join("installed");
        fs::create_dir(&installed).unwrap();
        std::os::unix::fs::symlink(owned.join("sprites"), installed.join("sprites")).unwrap();
        std::os::unix::fs::symlink(owned.join("sprites.txt"), installed.join("sprites.txt"))
            .unwrap();
        let addon = temp.path().join("addon/hd/3d/free");
        fs::create_dir_all(&addon).unwrap();
        fs::write(addon.join("manifest.json"), r#"{"models":[]}"#).unwrap();
        fs::write(addon.join("extra.txt"), b"addition").unwrap();
        install_hd(&temp.path().join("addon"), &installed, true, &reporter()).unwrap();
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"owned base"
        );
        assert!(installed.join("3d/free/extra.txt").is_file());
        assert!(!fs::symlink_metadata(installed.join("sprites"))
            .unwrap()
            .file_type()
            .is_symlink());
        assert!(!fs::symlink_metadata(installed.join("sprites.txt"))
            .unwrap()
            .file_type()
            .is_symlink());
        assert!(inventory(&installed, &reporter()).is_ok());
        assert_eq!(
            fs::read(owned.join("sprites/hero.png")).unwrap(),
            b"owned base"
        );
    }
    #[cfg(unix)]
    #[test]
    fn explicit_reference_seed_accepts_links_while_new_folder_import_rejects_them() {
        let temp = tempfile::tempdir().unwrap();
        let owned = temp.path().join("owned");
        base(&owned, b"reference source");
        let reference = temp.path().join("selected");
        std::os::unix::fs::symlink(&owned, &reference).unwrap();
        let installed = temp.path().join("installed");
        base(&installed, b"previous");
        assert!(install_hd(&reference, &installed, false, &reporter()).is_err());
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"previous"
        );
        copy_hd_reference(&reference, &installed, &reporter()).unwrap();
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"reference source"
        );
        assert!(assets::hd_status(&installed).is_ok());
        assert!(fs::symlink_metadata(&reference)
            .unwrap()
            .file_type()
            .is_symlink());
        assert!(inventory(&installed, &reporter()).is_ok());
    }
    #[cfg(unix)]
    #[test]
    fn reference_cycles_and_excessive_depth_preserve_the_previous_installation() {
        let temp = tempfile::tempdir().unwrap();
        let owned = temp.path().join("owned");
        base(&owned, b"new");
        let installed = temp.path().join("installed");
        base(&installed, b"previous");
        std::os::unix::fs::symlink(&owned, owned.join("loop")).unwrap();
        assert!(copy_hd_reference(&owned, &installed, &reporter())
            .unwrap_err()
            .contains("cycle"));
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"previous"
        );
        fs::remove_file(owned.join("loop")).unwrap();
        let mut deep = owned.clone();
        for _ in 0..=MAX_REFERENCE_DEPTH {
            deep = deep.join("d");
            fs::create_dir(&deep).unwrap();
        }
        assert!(copy_hd_reference(&owned, &installed, &reporter())
            .unwrap_err()
            .contains("64 directory levels"));
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"previous"
        );
    }
    #[cfg(unix)]
    #[test]
    fn existing_converted_symlinks_are_errors_instead_of_legacy_filename_rebuilds() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("glb");
        fs::create_dir(&source).unwrap();
        let target = temp.path().join("owned");
        fs::write(&target, b"source").unwrap();
        std::os::unix::fs::symlink(&target, source.join("linked.png")).unwrap();
        let stage = temp.path().join("stage");
        assert!(copy_existing_converted(&source, &stage, &reporter())
            .unwrap_err()
            .contains("Symlinks are not imported"));
        assert!(!stage.exists());
    }
    #[test]
    fn overlay_mode_accepts_a_fresh_complete_base_folder_and_zip() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source/hd");
        base(&source, b"fresh");
        let installed = temp.path().join("folder-install");
        install_hd(&temp.path().join("source"), &installed, true, &reporter()).unwrap();
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"fresh"
        );

        let archive = temp.path().join("base.zip");
        zip_file(
            &archive,
            &[
                (
                    "pack/hd/sprites.txt",
                    b"clip hero sprites/hero.png 1 1 1 1 0 0 1\n",
                ),
                ("pack/hd/sprites/hero.png", b"fresh-zip"),
            ],
        );
        let installed = temp.path().join("zip-install");
        install_hd(&archive, &installed, true, &reporter()).unwrap();
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"fresh-zip"
        );
    }
    #[test]
    fn addon_without_a_base_does_not_publish_an_incomplete_installation() {
        let temp = tempfile::tempdir().unwrap();
        let addon = temp.path().join("addon/hd/3d/free");
        fs::create_dir_all(&addon).unwrap();
        fs::write(addon.join("manifest.json"), r#"{"models":[]}"#).unwrap();
        let installed = temp.path().join("installed");
        assert!(install_hd(&temp.path().join("addon"), &installed, true, &reporter()).is_err());
        assert!(!installed.exists());
    }
    #[cfg(unix)]
    #[test]
    fn folder_symlink_is_rejected_without_changing_installation() {
        let temp = tempfile::tempdir().unwrap();
        let installed = temp.path().join("installed");
        base(&installed, b"original");
        let source = temp.path().join("source");
        base(&source, b"new");
        std::os::unix::fs::symlink(temp.path(), source.join("escape")).unwrap();
        assert!(install_hd(&source, &installed, false, &reporter()).is_err());
        assert_eq!(
            fs::read(installed.join("sprites/hero.png")).unwrap(),
            b"original"
        );
    }
    #[test]
    fn archive_case_collision_is_rejected() {
        let temp = tempfile::tempdir().unwrap();
        let archive = temp.path().join("collision.zip");
        zip_file(&archive, &[("hd/A.txt", b"a"), ("hd/a.txt", b"b")]);
        assert!(unzip(&archive, &temp.path().join("out"), &reporter()).is_err());
    }
    #[test]
    fn archive_directory_case_collision_is_rejected_before_extraction() {
        let temp = tempfile::tempdir().unwrap();
        let archive = temp.path().join("collision.zip");
        let mut zip = zip::ZipWriter::new(File::create(&archive).unwrap());
        zip.add_directory("hd/Models/", zip::write::SimpleFileOptions::default())
            .unwrap();
        zip.add_directory("hd/models/", zip::write::SimpleFileOptions::default())
            .unwrap();
        zip.finish().unwrap();
        let output = temp.path().join("out");
        assert!(unzip(&archive, &output, &reporter()).is_err());
        assert!(!output.exists());
        zip_file(
            &archive,
            &[("hd/Models/a.txt", b"a"), ("hd/models/b.txt", b"b")],
        );
        assert!(unzip(&archive, &output, &reporter()).is_err());
        assert!(!output.exists());
    }
    #[test]
    fn importer_zero_exit_does_not_hide_failed_work() {
        for line in [
            "FAIL hero conversion",
            "12 converted, 3 failed",
            "4 problems",
        ] {
            assert!(incomplete_conversion(line));
        }
        for line in ["12 converted, 0 failed", "0 problems", "Import complete"] {
            assert!(!incomplete_conversion(line));
        }
    }
    #[test]
    fn failure_reason_prefers_the_converters_error_line_over_progress() {
        let mut reason = FailureReason::default();
        for line in [
            "[1/114] Converting hero",
            "warning: slow disk",
            "Reusing complete model x",
        ] {
            reason.observe(line);
        }
        assert_eq!(reason.line(), Some("warning: slow disk"));
        reason.observe("d2r-convert: error: mapping-all: d2rtool could not open the D2R archives");
        reason.observe("[2/114] Converting rogue");
        assert_eq!(
            reason.line(),
            Some("d2r-convert: error: mapping-all: d2rtool could not open the D2R archives")
        );
        assert!(windows_status_meaning(0xC000_0135)
            .unwrap()
            .contains("Visual C++"));
    }
    /// A failing converter's whole output lands in the log file, and the notice
    /// leads with its error line and names the log instead of a bare exit code.
    #[cfg(unix)]
    #[test]
    fn failed_converter_reports_its_reason_and_log_file() {
        use std::os::unix::fs::PermissionsExt;
        let temp = tempfile::tempdir().unwrap();
        let program = temp.path().join("d2r-convert");
        fs::write(
            &program,
            "#!/bin/sh\necho '[1/2] Converting hero' >&2\necho stdout line\necho 'd2r-convert: error: mapping-all: Cannot write /x: No space left on device' >&2\nexit 1\n",
        )
        .unwrap();
        fs::set_permissions(&program, fs::Permissions::from_mode(0o755)).unwrap();
        let log_path = temp.path().join("launcher/d2r-convert.log");
        let log = ProcessLog::create(&log_path, &reporter()).unwrap();
        let error = run(
            &program,
            &["mapping-all".into()],
            temp.path(),
            &[],
            &reporter(),
            Some(&log),
        )
        .unwrap_err();
        let mut lines = error.lines();
        assert_eq!(
            lines.next(),
            Some("d2r-convert: error: mapping-all: Cannot write /x: No space left on device")
        );
        let exit = lines.next().unwrap();
        assert!(
            exit.starts_with("d2r-convert exited with exit status: 1."),
            "{exit}"
        );
        assert!(
            exit.ends_with(&format!("Full log: {}", log_path.display())),
            "{exit}"
        );
        let written = fs::read_to_string(&log_path).unwrap();
        for expected in [
            "Running ",
            "[1/2] Converting hero",
            "stdout line",
            "No space left",
            "exit status: 1",
        ] {
            assert!(
                written.contains(expected),
                "{expected} missing from {written}"
            );
        }
    }
    #[cfg(unix)]
    #[test]
    fn rust_importer_transaction_validates_phases_and_preserves_published_assets_on_failure() {
        use std::os::unix::fs::PermissionsExt;
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().canonicalize().unwrap();
        let game = root.join("owned game");
        let cache = root.join("cache");
        let importer = root.join("recipes");
        fs::create_dir_all(game.join("Data")).unwrap();
        fs::write(game.join(".build.info"), b"build").unwrap();
        fs::create_dir_all(importer.join("d2r_import")).unwrap();
        fs::write(importer.join("d2r_import/__main__.py"), b"").unwrap();
        fs::write(importer.join("requirements.txt"), b"").unwrap();
        fs::write(importer.join("mapping.toml"), b"fixture").unwrap();
        fs::write(importer.join("env_mapping.toml"), b"fixture").unwrap();
        fs::create_dir_all(cache.join("bin")).unwrap();
        fs::write(cache.join("bin/d2rtool"), b"helper").unwrap();
        let write = |phase: &str, path: &str, bytes: &[u8]| {
            let path = game.join("fixtures").join(phase).join(path);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        };
        let mut model = br#"{"asset":{"version":"2.0"},"animations":[{"name":"idle"}]}"#.to_vec();
        while model.len() % 4 != 0 {
            model.push(b' ');
        }
        let mut glb = b"glTF".to_vec();
        glb.extend_from_slice(&2u32.to_le_bytes());
        glb.extend_from_slice(&(20u32 + model.len() as u32).to_le_bytes());
        glb.extend_from_slice(&(model.len() as u32).to_le_bytes());
        glb.extend_from_slice(b"JSON");
        glb.extend(model);
        write("mapping-all", "hero.glb", &glb);
        write("mapping-all","mapping.resolved.json",br#"{"version":2,"entries":[{"model":"hero","glb":"hero.glb","clips":{"stand":"idle"}}]}"#);
        write("environment", "env/stone.png", b"image");
        write("environment","env/env.resolved.json",br#"{"version":2,"levels":{"town":{},"cathedral":{},"catacombs":{},"caves":{},"hell":{}},"models":{"hero":{"glb":"hero.glb"}},"materials":{"stone":{"albedo":"env/stone.png"}}}"#);
        write("ui", "ui/interface.png", b"image");
        write("ui","ui/ui.resolved.json",br#"{"version":1,"images":{"panel_background":{"file":"interface.png"},"button_sprint":{"file":"interface.png"},"options_background":{"file":"interface.png"},"options_pause_button":{"file":"interface.png"},"options_tabs":{"file":"interface.png"},"options_checkbox":{"file":"interface.png"},"options_slider_track":{"file":"interface.png"},"options_slider_thumb":{"file":"interface.png"}}}"#);
        write("portal", "vfx/town_portal/field.png", b"image");
        write("portal","vfx/town_portal/portal.resolved.json",br#"{"version":1,"textures":{"t_fx_noise_cells_conviction":{"png":"vfx/town_portal/field.png"},"t_fx_noise_cells_conviction_glow_02_hilights":{"png":"vfx/town_portal/field.png"},"t_fx_noise_goopy_03_meshdissolve":{"png":"vfx/town_portal/field.png"},"t_fx_portal_gradient_03":{"png":"vfx/town_portal/field.png"}}}"#);
        fs::create_dir_all(importer.join("bin")).unwrap();
        let converter = importer.join("bin/d2r-convert");
        fs::write(&converter, "#!/bin/sh\nif [ \"$1\" = environment ] && [ -f \"$2/fail-env\" ]; then exit 7; fi\nmkdir -p \"$3/glb\"\ncp -R \"$2/fixtures/$1/.\" \"$3/glb/\"\n").unwrap();
        fs::set_permissions(&converter, fs::Permissions::from_mode(0o755)).unwrap();
        let request = D2rImportRequest {
            game_dir: game.clone(),
            cache_dir: cache.clone(),
            importer_dir: importer,
            log_file: None,
        };
        let dest = import_d2r(&request, &reporter()).unwrap();
        let original = fs::read(dest.join("mapping.resolved.json")).unwrap();
        fs::write(game.join("fail-env"), b"").unwrap();
        // Change the source metadata so the resumed environment cannot be reused.
        fs::write(game.join(".build.info"), b"new build").unwrap();
        assert!(import_d2r_with_converter(&request, &reporter(), Some(converter)).is_err());
        assert_eq!(
            fs::read(dest.join("mapping.resolved.json")).unwrap(),
            original
        );
    }
    #[test]
    #[ignore = "requires owned D2R installation and compiled Rust converter/helper"]
    fn owned_rust_import_completes_through_launcher() {
        let game =
            PathBuf::from(std::env::var_os("D2R_RUST_OWNED_GAME").expect("D2R_RUST_OWNED_GAME"));
        let converter = PathBuf::from(
            std::env::var_os("D2R_RUST_TEST_CONVERTER").expect("D2R_RUST_TEST_CONVERTER"),
        );
        let helper =
            PathBuf::from(std::env::var_os("D2R_RUST_TEST_HELPER").expect("D2R_RUST_TEST_HELPER"));
        let importer = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../d2r-import")
            .canonicalize()
            .unwrap();
        let temp = tempfile::tempdir().unwrap();
        let cache = temp.path().join("d2r-models");
        fs::create_dir_all(cache.join("bin")).unwrap();
        let name = if cfg!(windows) {
            "d2rtool.exe"
        } else {
            "d2rtool"
        };
        fs::copy(helper, cache.join("bin").join(name)).unwrap();
        let request = D2rImportRequest {
            game_dir: game,
            cache_dir: cache,
            importer_dir: importer,
            log_file: None,
        };
        let dest = import_d2r_with_converter(&request, &reporter(), Some(converter)).unwrap();
        let status = assets::inspect(&AssetPaths {
            d2r_dir: Some(dest),
            ..Default::default()
        });
        assert!(
            status.d2r_ready(),
            "mapping: {}; environment: {}; ui: {}; portal: {}",
            status.d2r_mapping.detail,
            status.d2r_environment.detail,
            status.d2r_ui.detail,
            status.d2r_portal.detail
        );
    }
    #[test]
    fn process_child_fixture() {
        let Ok(mode) = std::env::var("DAMNATIONX_LAUNCHER_CHILD_TEST") else {
            return;
        };
        if mode == "parent" {
            let mut child = Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "jobs::tests::process_child_fixture",
                    "--nocapture",
                ])
                .env("DAMNATIONX_LAUNCHER_CHILD_TEST", "grandchild")
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .unwrap();
            for _ in 0..200 {
                thread::sleep(Duration::from_millis(25));
            }
            let _ = child.wait();
        } else {
            let file = PathBuf::from(std::env::var_os("DAMNATIONX_LAUNCHER_HEARTBEAT").unwrap());
            for index in 0..200 {
                fs::write(&file, index.to_string()).unwrap();
                thread::sleep(Duration::from_millis(25));
            }
        }
    }
    #[test]
    fn cancellation_stops_child_and_grandchild_without_a_shell() {
        let temp = tempfile::tempdir().unwrap();
        let heartbeat = temp.path().join("heartbeat");
        let worker_reporter = reporter();
        let cancel = worker_reporter.cancel.clone();
        let cwd = temp.path().to_path_buf();
        let path = heartbeat.clone();
        let thread = thread::spawn(move || {
            run(
                &std::env::current_exe().unwrap(),
                &[
                    "--exact".into(),
                    "jobs::tests::process_child_fixture".into(),
                    "--nocapture".into(),
                ],
                &cwd,
                &[
                    ("DAMNATIONX_LAUNCHER_CHILD_TEST".into(), "parent".into()),
                    (
                        "DAMNATIONX_LAUNCHER_HEARTBEAT".into(),
                        path.into_os_string(),
                    ),
                ],
                &worker_reporter,
                None,
            )
        });
        let deadline = Instant::now() + Duration::from_secs(4);
        while !heartbeat.is_file() && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(20));
        }
        cancel.store(true, Ordering::Release);
        let stopped = Instant::now();
        assert!(thread.join().unwrap().is_err());
        assert!(stopped.elapsed() < Duration::from_secs(2));
        assert!(heartbeat.is_file(), "owned grandchild never started");
        let before = fs::read(&heartbeat).unwrap();
        thread::sleep(Duration::from_millis(150));
        assert_eq!(
            fs::read(&heartbeat).unwrap(),
            before,
            "a helper survived cancellation"
        );
    }
}
