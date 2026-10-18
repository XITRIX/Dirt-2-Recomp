// Save-game sync through iCloud Drive (the app's ubiquity container, hidden
// from the Files app).
//
// The savegame folder is one consistent set of files, so it travels as one
// snapshot file (Saves/snapshot.pack: a header with the save time and source
// device, then the files). One file moves through iCloud atomically; a folder
// could arrive half-updated on the other device.
//
// Rules (each device remembers the snapshot time it last pulled or pushed):
//   - Nothing is uploaded unless iCloud's state is known (NSMetadataQuery has
//     gathered), and never over a snapshot this device has not seen: another
//     device's newer progress is never overwritten.
//   - At launch, before the game reads its saves: if only the cloud changed
//     since the last sync (a new device has no saves at all), its snapshot is
//     pulled; if both changed, the user chooses. Replaced saves go to
//     savegame.bak.
//   - After the game writes a save (files quiet for 2 s), on going to the
//     background and at exit, local changes are uploaded.
// Without iCloud (signed out, simulator, macOS without an app bundle) this is a no-op.
#include <TargetConditionals.h>
#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../platform.h"
#include "recomp/host.h"

namespace fs = std::filesystem;

namespace platform {

namespace {

constexpr const char* kMagic = "DIRT2SAVE1";

std::mutex g_mu;           // one sync operation at a time
fs::path g_local;          // .../savegame
NSURL* g_container = nil;  // iCloud container root
double g_synced = 0;       // snapshot time last pulled or pushed by this device
std::atomic<bool> g_enabled{false};

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSURL* pack_url() {
    return [[g_container URLByAppendingPathComponent:@"Saves" isDirectory:YES] URLByAppendingPathComponent:@"snapshot.pack"];
}

std::string device_name() {
#if TARGET_OS_IPHONE
    __block std::string name;
    auto get = ^{ name = UIDevice.currentDevice.model.UTF8String ?: "iOS device"; };
    if (NSThread.isMainThread) get();
    else dispatch_sync(dispatch_get_main_queue(), get);
    return name;
#else
    return NSProcessInfo.processInfo.hostName.UTF8String ?: "Mac";
#endif
}

std::string when(double t) {
    char buf[64];
    const time_t s = time_t(t);
    std::strftime(buf, sizeof buf, "%d %b %H:%M", std::localtime(&s));
    return buf;
}

// Newest modification time (seconds since 1970) of any file in the folder.
double newest_mtime(const fs::path& dir) {
    double t = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ft = it->last_write_time(ec);
        if (ec) continue;
        t = std::max(t, std::chrono::duration<double>(std::chrono::file_clock::to_sys(ft).time_since_epoch()).count());
    }
    return t;
}

// Format 2 (snapshot.pack); the record of format 1 (a folder plus snapshot.txt)
// is ignored, so every device uploads or pulls the new format once.
fs::path state_file() { return g_local.parent_path() / "savegame.cloud2"; }
void save_state() { std::ofstream(state_file()) << std::fixed << g_synced << "\n"; }
void load_state() {
    std::ifstream in(state_file());
    if (!(in >> g_synced)) g_synced = 0;
}

// ---- snapshot file: "DIRT2SAVE1 <time> <device>\n" then per file
// "<relative path>\n<size>\n<bytes>" ----

std::string pack_folder(const fs::path& dir, double t) {
    std::ostringstream out;
    out << kMagic << " " << std::fixed << t << " " << device_name() << "\n";
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::ifstream f(it->path(), std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(f)), {});
        out << fs::relative(it->path(), dir).generic_string() << "\n" << data.size() << "\n" << data;
    }
    return out.str();
}

struct Snapshot {
    double time = 0;
    std::string device;
    std::vector<std::pair<std::string, std::string>> files;
};

bool parse_pack(const std::string& blob, Snapshot& s, bool header_only) {
    std::istringstream in(blob);
    std::string magic, line;
    if (!(in >> magic >> s.time) || magic != kMagic) return false;
    std::getline(in, s.device);
    if (!s.device.empty() && s.device[0] == ' ') s.device.erase(0, 1);
    if (header_only) return true;
    while (std::getline(in, line) && !line.empty()) {
        size_t size = 0;
        std::string size_line;
        if (!std::getline(in, size_line)) return false;
        size = std::stoul(size_line);
        std::string data(size, '\0');
        if (!in.read(data.data(), std::streamsize(size))) return false;
        if (line.find("..") != std::string::npos || line[0] == '/') return false;
        s.files.emplace_back(line, std::move(data));
    }
    return true;
}

// ---- iCloud access ----

// What iCloud's index says about the snapshot: known == false when the index
// could not be gathered in time (then nothing may be uploaded).
struct CloudState {
    bool known = false;
    bool exists = false;
};

CloudState query_cloud(double timeout) {
    __block CloudState st;
    __block bool finished = false;
    NSMetadataQuery* q = [[NSMetadataQuery alloc] init];
    q.searchScopes = @[ NSMetadataQueryUbiquitousDataScope ];
    q.predicate = [NSPredicate predicateWithFormat:@"%K == %@", NSMetadataItemFSNameKey, @"snapshot.pack"];
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    q.operationQueue = queue;
    id token = [NSNotificationCenter.defaultCenter addObserverForName:NSMetadataQueryDidFinishGatheringNotification
                                                               object:q
                                                                queue:queue
                                                           usingBlock:^(NSNotification*) {
                                                               [q disableUpdates];
                                                               st.known = true;
                                                               st.exists = q.resultCount > 0;
                                                               finished = true;
                                                           }];
    [queue addOperationWithBlock:^{ [q startQuery]; }];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (!finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    [queue addOperationWithBlock:^{ [q stopQuery]; }];
    [queue waitUntilAllOperationsAreFinished];
    [NSNotificationCenter.defaultCenter removeObserver:token];
    return st;
}

// Downloads the snapshot (if needed) and reads it.
bool read_pack(std::string& blob, double timeout) {
    NSURL* url = pack_url();
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm startDownloadingUbiquitousItemAtURL:url error:nil];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    for (;;) {
        [url removeAllCachedResourceValues];
        NSString* status = nil;
        [url getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
        if ([status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]) break;
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    __block bool ok = false;
    __block std::string data;
    NSError* err = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
        coordinateReadingItemAtURL:url
                           options:0
                             error:&err
                        byAccessor:^(NSURL* u) {
                            NSData* d = [NSData dataWithContentsOfURL:u];
                            if (d) data.assign(static_cast<const char*>(d.bytes), d.length), ok = true;
                        }];
    blob = std::move(data);
    return ok;
}

bool write_pack(const std::string& blob) {
    NSURL* url = pack_url();
    [NSFileManager.defaultManager createDirectoryAtURL:url.URLByDeletingLastPathComponent
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
    __block bool ok = false;
    NSError* err = nil;
    NSData* d = [NSData dataWithBytes:blob.data() length:blob.size()];
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
        coordinateWritingItemAtURL:url
                           options:NSFileCoordinatorWritingForReplacing
                             error:&err
                        byAccessor:^(NSURL* u) { ok = [d writeToURL:u atomically:YES]; }];
    if (!ok) rt::log("cloud saves: upload failed: %s", err.localizedDescription.UTF8String ?: "write failed");
    return ok;
}

// Replaces the local saves with the snapshot; the old ones go to savegame.bak.
bool apply_snapshot(const Snapshot& s) {
    const fs::path tmp = g_local.parent_path() / "savegame.cloud-tmp";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    for (const auto& [rel, data] : s.files) {
        const fs::path p = tmp / rel;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary).write(data.data(), std::streamsize(data.size()));
    }
    const fs::path bak = g_local.parent_path() / "savegame.bak";
    fs::remove_all(bak, ec);
    if (fs::exists(g_local)) fs::rename(g_local, bak, ec);
    fs::rename(tmp, g_local, ec);
    if (ec) return false;
    // The written files are newer than the snapshot time: record what is on disk
    // now, so they do not count as local changes.
    g_synced = std::max(s.time, newest_mtime(g_local));
    save_state();
    rt::log("cloud saves: using the save from %s (%s); previous local saves in savegame.bak", s.device.c_str(),
            when(s.time).c_str());
    return true;
}

// Uploads local changes, unless iCloud's state is unknown or holds a snapshot
// this device has not seen (force: the user chose this device's saves).
void push_locked(bool force = false) {
    const double local = newest_mtime(g_local);
    if (local == 0 || local <= g_synced) return;
    const CloudState st = query_cloud(10);
    if (!st.known) {
        rt::log("cloud saves: iCloud state unknown, upload postponed");
        return;
    }
    if (st.exists && !force) {
        std::string blob;
        Snapshot s;
        if (!read_pack(blob, 10) || !parse_pack(blob, s, true)) {
            rt::log("cloud saves: cannot read the iCloud snapshot, upload postponed");
            return;
        }
        if (s.time > g_synced) {
            rt::log("cloud saves: %s saved since this device last synced; not uploading over it (resolved at next launch)",
                    s.device.c_str());
            return;
        }
    }
    if (!write_pack(pack_folder(g_local, local))) return;
    g_synced = local;
    save_state();
    rt::log("cloud saves: uploaded the save from %s", when(local).c_str());
}

// At launch: decides between the cloud snapshot and the local saves.
void reconcile_locked() {
    const double local = newest_mtime(g_local);
    const bool local_changed = local > g_synced;
    const CloudState st = query_cloud(15);
    if (!st.known) {
        rt::log("cloud saves: iCloud state unknown at launch, using local saves (no upload until known)");
        return;
    }
    if (!st.exists) {
        rt::log("cloud saves: no iCloud save yet");
        return;
    }
    std::string blob;
    Snapshot s;
    if (!read_pack(blob, 20) || !parse_pack(blob, s, false)) {
        rt::log("cloud saves: iCloud save did not download, using local saves");
        return;
    }
    rt::log("cloud saves: iCloud save from %s (%s), local %s, last synced %s", s.device.c_str(), when(s.time).c_str(),
            local ? when(local).c_str() : "none", g_synced ? when(g_synced).c_str() : "never");
    if (s.time <= g_synced) return;  // nothing new in the cloud
    if (!local_changed || local == 0) {
        apply_snapshot(s);
        return;
    }
    // Both sides have progress the other has not seen: the user decides.
    const std::string msg = "iCloud has a save from " + s.device + " (" + when(s.time) + ").\nThis device has a save from " +
                            when(local) + ".\n\nThe one you do not use is kept as savegame.bak.";
    const int pick = platform::choose("DiRT 2: two different saves", msg, "Use iCloud save", "Keep this device's save");
    if (pick == 0) apply_snapshot(s);
    else push_locked(true);
}

void watch() {
    double seen = -1;
    auto changed_at = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double t = newest_mtime(g_local);
        const auto now = std::chrono::steady_clock::now();
        if (t != seen) {
            seen = t;
            changed_at = now;
            continue;
        }
        // A save is several files: wait until they have been quiet for 2 s.
        if (t > g_synced && now - changed_at >= std::chrono::seconds(2)) {
            std::lock_guard lk(g_mu);
            push_locked();
        }
    }
}

}  // namespace

void cloud_saves_start(const std::string& local_dir) {
    g_local = local_dir;
    NSURL* container = [NSFileManager.defaultManager URLForUbiquityContainerIdentifier:nil];
    if (!container) {
        rt::log("cloud saves: iCloud Drive not available, saves stay on this device");
        return;
    }
    g_container = container;
    g_enabled = true;
    {
        std::lock_guard lk(g_mu);
        load_state();
        std::error_code ec;
        fs::create_directories(g_local, ec);
        reconcile_locked();
    }
    std::thread([] {
        {
            std::lock_guard lk(g_mu);
            push_locked();
        }
        watch();
    }).detach();
    std::atexit([] { cloud_saves_flush(); });
}

void cloud_saves_flush() {
    if (!g_enabled) return;
    auto push = [] {
        std::lock_guard lk(g_mu);
        push_locked();
    };
#if TARGET_OS_IPHONE
    if (NSThread.isMainThread) {
        // Going to the background: upload on a worker with background time, so the
        // main thread (and the app's suspension) is not held up.
        __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
        task = [UIApplication.sharedApplication beginBackgroundTaskWithExpirationHandler:^{
            [UIApplication.sharedApplication endBackgroundTask:task];
        }];
        const UIBackgroundTaskIdentifier id = task;
        std::thread([push, id] {
            push();
            dispatch_async(dispatch_get_main_queue(), ^{
                if (id != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:id];
            });
        }).detach();
        return;
    }
#endif
    push();
}

}  // namespace platform
