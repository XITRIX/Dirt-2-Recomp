// Save-game sync through CloudKit (the user's private database).
//
// The savegame folder is one consistent set of files, so it travels as one
// snapshot (a header with the save time and source device, then the files) in
// a single record, Save/"snapshot": fields time, device, and the packed folder
// as an asset. CloudKit answers authoritatively: the record exists, does not
// exist, or the request failed (state unknown). iCloud Drive could not: on a
// freshly installed device its index said "no file" before it had synced.
//
// Rules (each device remembers the snapshot time it last pulled or pushed):
//   - Nothing is uploaded while the state is unknown, and never over a
//     snapshot this device has not seen; the save also carries the record's
//     change tag, so the server rejects a concurrent overwrite.
//   - At launch, before the game reads its saves: if only the cloud changed
//     since the last sync (a new device has no saves at all), its snapshot is
//     pulled; if both changed, the user chooses. Replaced saves go to
//     savegame.bak.
//   - After the game writes a save (files quiet for 2 s), on going to the
//     background and at exit, local changes are uploaded.
// Without an iCloud account (or entitlement: simulator, macOS without an app
// bundle) this is a no-op.
#include <TargetConditionals.h>
#import <CloudKit/CloudKit.h>
#import <Security/Security.h>
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

std::mutex g_mu;                 // one sync operation at a time
fs::path g_local;                // .../savegame
CKDatabase* g_db = nil;          // private database of the app's container
CKRecord* g_record = nil;        // last fetched snapshot record (its change tag guards the next save)
double g_synced = 0;       // snapshot time last pulled or pushed by this device
std::atomic<bool> g_enabled{false};
bool g_playing = false;          // the game is running (conflicts pause it and ask)

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }


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

// The records of earlier iCloud Drive versions (savegame.cloud, .cloud2) are
// ignored, so every device uploads or pulls the CloudKit snapshot once.
fs::path state_file() { return g_local.parent_path() / "savegame.cloudkit"; }
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

// ---- CloudKit ----

CKRecordID* record_id() { return [[CKRecordID alloc] initWithRecordName:@"snapshot"]; }

enum class Fetch { Found, Missing, Unknown };

// Fetches the snapshot record (with its asset) from the server.
Fetch fetch_record(CKRecord* __strong& out, double timeout) {
    __block CKRecord* rec = nil;
    __block NSError* error = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [g_db fetchRecordWithID:record_id()
          completionHandler:^(CKRecord* r, NSError* e) {
              rec = r;
              error = e;
              dispatch_semaphore_signal(done);
          }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(timeout * NSEC_PER_SEC))) != 0) {
        rt::log("cloud saves: iCloud did not answer in time");
        return Fetch::Unknown;
    }
    if (rec) {
        out = rec;
        return Fetch::Found;
    }
    if (error.code == CKErrorUnknownItem) return Fetch::Missing;
    rt::log("cloud saves: iCloud request failed: %s", error.localizedDescription.UTF8String ?: "?");
    return Fetch::Unknown;
}

bool record_snapshot(CKRecord* rec, Snapshot& s, bool with_files) {
    CKAsset* asset = rec[@"data"];
    NSData* d = asset.fileURL ? [NSData dataWithContentsOfURL:asset.fileURL] : nil;
    if (!d) return false;
    const std::string blob(static_cast<const char*>(d.bytes), d.length);
    return parse_pack(blob, s, !with_files);
}

// Saves the snapshot; rec is the last fetched record (nil when none exists),
// whose change tag makes the server reject an overwrite of a newer version.
bool save_record(CKRecord* rec, const std::string& blob, double t) {
    if (!rec) rec = [[CKRecord alloc] initWithRecordType:@"Save" recordID:record_id()];
    NSURL* file = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:@"dirt2-snapshot.pack"]];
    [[NSData dataWithBytes:blob.data() length:blob.size()] writeToURL:file atomically:YES];
    rec[@"data"] = [[CKAsset alloc] initWithFileURL:file];
    rec[@"time"] = @(t);
    rec[@"device"] = ns(device_name());
    __block CKRecord* saved = nil;
    __block NSError* error = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CKModifyRecordsOperation* op = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:@[ rec ] recordIDsToDelete:nil];
    op.savePolicy = CKRecordSaveIfServerRecordUnchanged;
    op.qualityOfService = NSQualityOfServiceUserInitiated;
    op.modifyRecordsCompletionBlock = ^(NSArray<CKRecord*>* records, NSArray<CKRecordID*>*, NSError* e) {
        saved = records.firstObject;
        // A per-record failure arrives wrapped in CKErrorPartialFailure.
        NSError* item = e.code == CKErrorPartialFailure ? e.userInfo[CKPartialErrorsByItemIDKey][record_id()] : nil;
        error = item ?: e;
        dispatch_semaphore_signal(done);
    };
    [g_db addOperation:op];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC)) != 0) {
        rt::log("cloud saves: upload timed out");
        return false;
    }
    if (error || !saved) {
        if (error.code == CKErrorServerRecordChanged)
            rt::log("cloud saves: another device saved first; not overwriting it");
        else
            rt::log("cloud saves: upload failed: %s", error.localizedDescription.UTF8String ?: "?");
        return false;
    }
    g_record = saved;
    return true;
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

void push_locked(bool force);

// During play: another device saved since this device last synced (Steam's
// "cloud out of sync"). The game is paused until the user decides.
void conflict_during_play_locked(CKRecord* rec) {
    static double asked = 0;  // ask once per conflicting snapshot
    const double t = [rec[@"time"] doubleValue];
    if (t == asked) return;
    asked = t;
    Snapshot s;
    if (!record_snapshot(rec, s, true)) return;
    platform::set_game_paused(true);
    const std::string msg = "Your " + s.device + " saved progress to iCloud (" + when(s.time) +
                            ") that this device has not loaded.\nThis device's save is from " +
                            when(newest_mtime(g_local)) + ".\n\nThe one you do not use is kept as savegame.bak.";
    const int pick = platform::choose("Cloud save conflict", msg, "Keep this device's save", "Use iCloud save");
    if (pick == 0) {
        push_locked(true);
        platform::set_game_paused(false);
        return;
    }
    // The game holds its save in memory: put the iCloud one in place and close,
    // so the next launch loads it (iOS apps cannot relaunch themselves).
    apply_snapshot(s);
    platform::choose("DiRT 2", "The iCloud save is in place. The game will close now; open it again to continue from it.",
                     "Close", "Close");
    std::_Exit(0);
}

// Uploads local changes, unless the cloud state is unknown or holds a snapshot
// this device has not seen (force: the user chose this device's saves).
void push_locked(bool force) {
    const double local = newest_mtime(g_local);
    if (local == 0 || local <= g_synced) return;
    CKRecord* rec = nil;
    const Fetch f = fetch_record(rec, 20);
    if (f == Fetch::Unknown) {
        rt::log("cloud saves: iCloud state unknown, upload postponed");
        return;
    }
    if (f == Fetch::Found && !force) {
        const double t = [rec[@"time"] doubleValue];
        if (t > g_synced) {
            rt::log("cloud saves: %s saved since this device last synced; not uploading over it",
                    [rec[@"device"] UTF8String] ?: "another device");
            if (g_playing) conflict_during_play_locked(rec);
            return;
        }
    }
    if (!save_record(rec, pack_folder(g_local, local), local)) return;
    g_synced = local;
    save_state();
    rt::log("cloud saves: uploaded the save from %s", when(local).c_str());
}

// At launch: decides between the cloud snapshot and the local saves.
void reconcile_locked() {
    const double local = newest_mtime(g_local);
    const bool local_changed = local > g_synced;
    CKRecord* rec = nil;
    const Fetch f = fetch_record(rec, 20);
    if (f == Fetch::Unknown) {
        rt::log("cloud saves: iCloud state unknown at launch, using local saves (no upload until known)");
        return;
    }
    if (f == Fetch::Missing) {
        rt::log("cloud saves: no iCloud save yet");
        return;
    }
    g_record = rec;
    Snapshot s;
    if (!record_snapshot(rec, s, true)) {
        rt::log("cloud saves: iCloud save unreadable, using local saves");
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
    const int pick = platform::choose("Cloud save conflict", msg, "Use iCloud save", "Keep this device's save");
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
            push_locked(false);
        }
    }
}

}  // namespace

namespace {

NSString* const kContainer = @"iCloud.com.daniilvinogradov.dirt2recomp";

// Whether the code signature grants our iCloud container.
bool has_icloud_entitlement() {
#if TARGET_OS_OSX
    SecTaskRef task = SecTaskCreateFromSelf(nullptr);
    if (!task) return false;
    CFTypeRef v = SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.developer.icloud-container-identifiers"), nullptr);
    CFRelease(task);
    if (!v) return false;
    bool ok = false;
    if (CFGetTypeID(v) == CFArrayGetTypeID())
        ok = [(__bridge NSArray*)v containsObject:kContainer];
    CFRelease(v);
    return ok;
#else
    // iOS has no SecTask API: the provisioning profile the app was signed with
    // lists the containers it grants.
    NSString* path = [NSBundle.mainBundle pathForResource:@"embedded" ofType:@"mobileprovision"];
    NSData* d = path ? [NSData dataWithContentsOfFile:path] : nil;
    if (!d) return false;
    NSData* needle = [kContainer dataUsingEncoding:NSUTF8StringEncoding];
    return [d rangeOfData:needle options:0 range:NSMakeRange(0, d.length)].location != NSNotFound;
#endif
}

}  // namespace

void cloud_saves_start(const std::string& local_dir) {
    g_local = local_dir;
    // CloudKit needs the entitlement (absent: simulator builds, the bare macOS
    // executable) and a signed-in iCloud account.
    if (![NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleIdentifier"]) {
        rt::log("cloud saves: no app bundle, saves stay on this device");
        return;
    }
    // Without the iCloud entitlement (an ad hoc signed .app, an .ipa signed by
    // someone else) CloudKit traps (SIGTRAP) rather than failing: check first.
    if (!has_icloud_entitlement()) {
        rt::log("cloud saves: this build has no iCloud entitlement, saves stay on this device");
        return;
    }
    CKContainer* container = [CKContainer containerWithIdentifier:kContainer];
    __block CKAccountStatus status = CKAccountStatusCouldNotDetermine;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [container accountStatusWithCompletionHandler:^(CKAccountStatus st, NSError*) {
        status = st;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    if (status != CKAccountStatusAvailable) {
        rt::log("cloud saves: no iCloud account available (%ld), saves stay on this device", long(status));
        return;
    }
    g_db = container.privateCloudDatabase;
    g_enabled = true;
    {
        std::lock_guard lk(g_mu);
        load_state();
        std::error_code ec;
        fs::create_directories(g_local, ec);
        reconcile_locked();
        g_playing = true;
    }
    std::thread([] {
        {
            std::lock_guard lk(g_mu);
            push_locked(false);
        }
        watch();
    }).detach();
    std::atexit([] { cloud_saves_flush(); });
}

void cloud_saves_flush() {
    if (!g_enabled) return;
    auto push = [] {
        std::lock_guard lk(g_mu);
        push_locked(false);
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
