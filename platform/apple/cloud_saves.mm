// Save-game sync through iCloud Drive (the app's ubiquity container, hidden
// from the Files app). The game's savegame folder is one consistent snapshot,
// so it moves as a whole:
//   - at launch, before the guest runs, a snapshot made on another device that
//     is newer than the local saves replaces them (the old ones go to
//     savegame.bak);
//   - after the game writes a save (files unchanged for 2 s), on going to the
//     background and at exit, a newer local snapshot is uploaded.
// Times are the newest file modification time in the snapshot. Without iCloud
// (signed out, simulator, macOS without an app bundle) everything is a no-op.
#include <TargetConditionals.h>
#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "../platform.h"
#include "recomp/host.h"

namespace fs = std::filesystem;

namespace platform {

namespace {

std::mutex g_mu;            // one sync operation at a time
fs::path g_local;           // .../savegame
NSURL* g_cloud_dir = nil;   // <container>/Saves
double g_synced = 0;        // snapshot time last pulled or pushed
std::atomic<bool> g_enabled{false};

NSString* ns(const fs::path& p) { return [NSString stringWithUTF8String:p.c_str()]; }

// Newest modification time (seconds since 1970) of any file in the folder.
double newest_mtime(const fs::path& dir) {
    double t = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ft = it->last_write_time(ec);
        if (ec) continue;
        const auto sys = std::chrono::file_clock::to_sys(ft);
        t = std::max(t, std::chrono::duration<double>(sys.time_since_epoch()).count());
    }
    return t;
}

fs::path state_file() { return g_local.parent_path() / "savegame.cloud"; }

void save_state() {
    std::ofstream(state_file()) << std::fixed << g_synced << "\n";
}

void load_state() {
    std::ifstream in(state_file());
    if (!(in >> g_synced)) g_synced = 0;
}

// Asks iCloud to download an item (or a folder's items) and waits until it is current.
bool ensure_downloaded(NSURL* url, double timeout) {
    NSFileManager* fm = NSFileManager.defaultManager;
    NSMutableArray<NSURL*>* items = [NSMutableArray arrayWithObject:url];
    NSNumber* isDir = nil;
    [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    if (isDir.boolValue)
        for (NSURL* u in [fm enumeratorAtURL:url includingPropertiesForKeys:nil options:0 errorHandler:nil])
            [items addObject:u];
    for (NSURL* u in items) [fm startDownloadingUbiquitousItemAtURL:u error:nil];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    for (;;) {
        bool all = true;
        for (NSURL* u in items) {
            [u removeAllCachedResourceValues];
            NSString* status = nil;
            [u getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
            if (status && ![status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]) all = false;
        }
        if (all) return true;
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

double cloud_time() {
    NSURL* manifest = [g_cloud_dir URLByAppendingPathComponent:@"snapshot.txt"];
    ensure_downloaded(manifest, 5);
    if (![NSFileManager.defaultManager fileExistsAtPath:manifest.path]) return 0;  // no snapshot yet
    __block double t = 0;
    NSError* err = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
        coordinateReadingItemAtURL:manifest
                           options:0
                             error:&err
                        byAccessor:^(NSURL* u) {
                            NSString* s = [NSString stringWithContentsOfURL:u encoding:NSUTF8StringEncoding error:nil];
                            t = s.doubleValue;
                        }];
    return t;
}

// Copies the cloud snapshot over the local saves (keeping the old ones in savegame.bak).
bool pull_locked(double t) {
    NSURL* src = [g_cloud_dir URLByAppendingPathComponent:@"savegame" isDirectory:YES];
    if (!ensure_downloaded(src, 10)) {
        rt::log("cloud saves: snapshot did not finish downloading, keeping local saves");
        return false;
    }
    const fs::path tmp = g_local.parent_path() / "savegame.cloud-tmp";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    __block bool ok = false;
    NSError* err = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
        coordinateReadingItemAtURL:src
                           options:0
                             error:&err
                        byAccessor:^(NSURL* u) {
                            ok = [NSFileManager.defaultManager copyItemAtURL:u toURL:[NSURL fileURLWithPath:ns(tmp)]
                                                                       error:nil];
                        }];
    if (!ok) return false;
    const fs::path bak = g_local.parent_path() / "savegame.bak";
    fs::remove_all(bak, ec);
    if (fs::exists(g_local)) fs::rename(g_local, bak, ec);
    fs::rename(tmp, g_local, ec);
    if (ec) return false;
    g_synced = t;
    save_state();
    rt::log("cloud saves: pulled snapshot from %.0f (local saves kept in savegame.bak)", t);
    return true;
}

// Uploads the local saves when they are newer than the last synced snapshot.
void push_locked() {
    const double t = newest_mtime(g_local);
    if (t <= g_synced || t == 0) return;
    NSURL* dst = [g_cloud_dir URLByAppendingPathComponent:@"savegame" isDirectory:YES];
    NSURL* manifest = [g_cloud_dir URLByAppendingPathComponent:@"snapshot.txt"];
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm createDirectoryAtURL:g_cloud_dir withIntermediateDirectories:YES attributes:nil error:nil];
    __block bool ok = false;
    NSError* err = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
        coordinateWritingItemAtURL:dst
                           options:NSFileCoordinatorWritingForReplacing
                             error:&err
                        byAccessor:^(NSURL* u) {
                            [fm removeItemAtURL:u error:nil];
                            ok = [fm copyItemAtURL:[NSURL fileURLWithPath:ns(g_local)] toURL:u error:nil];
                        }];
    if (!ok) {
        rt::log("cloud saves: upload failed: %s", err.localizedDescription.UTF8String ?: "copy failed");
        return;
    }
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil]
        coordinateWritingItemAtURL:manifest
                           options:NSFileCoordinatorWritingForReplacing
                             error:&err
                        byAccessor:^(NSURL* u) {
                            [[NSString stringWithFormat:@"%.6f\n%@\n", t, NSProcessInfo.processInfo.hostName]
                                writeToURL:u
                                atomically:YES
                                  encoding:NSUTF8StringEncoding
                                     error:nil];
                        }];
    g_synced = t;
    save_state();
    rt::log("cloud saves: uploaded snapshot %.0f", t);
}

void watch() {
    double seen = 0;
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
    // The ubiquity container can take a while to resolve; never on the main thread.
    __block NSURL* container = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        container = [NSFileManager.defaultManager URLForUbiquityContainerIdentifier:nil];
        dispatch_semaphore_signal(done);
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC)) != 0 || !container) {
        rt::log("cloud saves: iCloud Drive not available, saves stay on this device");
        return;
    }
    g_cloud_dir = [container URLByAppendingPathComponent:@"Saves" isDirectory:YES];
    g_enabled = true;
    {
        std::lock_guard lk(g_mu);
        load_state();
        std::error_code ec;
        fs::create_directories(g_local, ec);
        const double cloud = cloud_time(), local = newest_mtime(g_local);
        rt::log("cloud saves: cloud snapshot %.0f, local %.0f, last synced %.0f", cloud, local, g_synced);
        // Another device saved since our last sync, and later than our own saves:
        // take them before the game reads its saves. Uploads need not hold up the start.
        if (cloud > g_synced && cloud > local) pull_locked(cloud);
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
#if TARGET_OS_IPHONE
    // Going to the background: ask for time to finish the upload.
    __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
    if (NSThread.isMainThread)
        task = [UIApplication.sharedApplication beginBackgroundTaskWithExpirationHandler:^{
            [UIApplication.sharedApplication endBackgroundTask:task];
        }];
#endif
    {
        std::lock_guard lk(g_mu);
        push_locked();
    }
#if TARGET_OS_IPHONE
    if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
#endif
}

}  // namespace platform
