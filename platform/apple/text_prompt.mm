// Native UI on Apple platforms. platform::prompt_text: an alert with a text
// field, shown on the main thread (where SDL pumps the run loop); on iOS it
// brings up the system keyboard. It stands in for the GFWL on-screen keyboard.
// platform::set_app_icon: the game's icon for the Dock and alerts on macOS,
// where the executable is not an app bundle.
#include <SDL3/SDL.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

#include "../platform.h"

namespace platform {

namespace {

struct Prompt {
    NSString* title;
    NSString* message;
    NSString* initial;
    dispatch_semaphore_t done;
    bool ok = false;
    std::string result;
};

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

#if TARGET_OS_IPHONE
UIWindow* g_alert_window = nil;  // before the game has a window

// The view controller to present an alert from.
UIViewController* presenter() {
    UIViewController* root = nil;
    UIWindowScene* any_scene = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class]) {
            any_scene = static_cast<UIWindowScene*>(scene);
            for (UIWindow* w in any_scene.windows)
                if (w.isKeyWindow) root = w.rootViewController;
        }
    if (!root && any_scene) {
        g_alert_window = [[UIWindow alloc] initWithWindowScene:any_scene];
        g_alert_window.rootViewController = [[UIViewController alloc] init];
        g_alert_window.windowLevel = UIWindowLevelAlert;
        [g_alert_window makeKeyAndVisible];
        root = g_alert_window.rootViewController;
    }
    while (root.presentedViewController) root = root.presentedViewController;
    return root;
}

void drop_alert_window() {
    g_alert_window.hidden = YES;
    g_alert_window = nil;
}
#endif

void show(void* userdata) {
    auto* p = static_cast<Prompt*>(userdata);
#if TARGET_OS_IPHONE
    UIViewController* root = presenter();
    if (!root) {
        dispatch_semaphore_signal(p->done);
        return;
    }
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:p->title
                                                                   message:p->message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
        field.text = p->initial;
        field.clearButtonMode = UITextFieldViewModeAlways;
        field.autocapitalizationType = UITextAutocapitalizationTypeWords;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction*) {
               drop_alert_window();
               dispatch_semaphore_signal(p->done);
           }]];
    UIAlertController* __weak weak = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
               p->result = weak.textFields.firstObject.text.UTF8String ?: "";
               p->ok = true;
               drop_alert_window();
               dispatch_semaphore_signal(p->done);
           }]];
    [root presentViewController:alert animated:YES completion:nil];
#else
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = p->title;
    alert.informativeText = p->message;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = p->initial;
    alert.accessoryView = field;
    alert.window.initialFirstResponder = field;
    if ([alert runModal] == NSAlertFirstButtonReturn) {
        p->result = field.stringValue.UTF8String ?: "";
        p->ok = true;
    }
    dispatch_semaphore_signal(p->done);
#endif
}

}  // namespace

bool prompt_text(const std::string& title, const std::string& message, const std::string& initial,
                 std::string& result) {
    Prompt p{ns(title), ns(message), ns(initial), dispatch_semaphore_create(0)};
    SDL_RunOnMainThread(show, &p, false);
    dispatch_semaphore_wait(p.done, DISPATCH_TIME_FOREVER);
    if (p.ok) result = p.result;
    return p.ok;
}

namespace {
struct Choice {
    NSString *title, *message, *first, *second;
    dispatch_semaphore_t done;
    int pick = 0;
};

void show_choice(void* userdata) {
    auto* p = static_cast<Choice*>(userdata);
#if TARGET_OS_IPHONE
    UIViewController* root = presenter();
    if (!root) {
        dispatch_semaphore_signal(p->done);
        return;
    }
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:p->title
                                                                   message:p->message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:p->first style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
               p->pick = 0;
               drop_alert_window();
               dispatch_semaphore_signal(p->done);
           }]];
    [alert addAction:[UIAlertAction actionWithTitle:p->second style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
               p->pick = 1;
               drop_alert_window();
               dispatch_semaphore_signal(p->done);
           }]];
    [root presentViewController:alert animated:YES completion:nil];
#else
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = p->title;
    alert.informativeText = p->message;
    [alert addButtonWithTitle:p->first];
    [alert addButtonWithTitle:p->second];
    p->pick = [alert runModal] == NSAlertFirstButtonReturn ? 0 : 1;
    dispatch_semaphore_signal(p->done);
#endif
}
}  // namespace

int choose(const std::string& title, const std::string& message, const std::string& first, const std::string& second) {
    Choice c{ns(title), ns(message), ns(first), ns(second), dispatch_semaphore_create(0)};
    SDL_RunOnMainThread(show_choice, &c, false);
    dispatch_semaphore_wait(c.done, DISPATCH_TIME_FOREVER);
    return c.pick;
}

void set_app_icon(const std::string& path) {
#if !TARGET_OS_IPHONE
    if (NSImage* icon = [[NSImage alloc] initWithContentsOfFile:ns(path)]) NSApp.applicationIconImage = icon;
#else
    (void)path;  // the app bundle has its own icon
#endif
}

bool choose_folder(const std::string& title, std::string& path) {
#if TARGET_OS_IPHONE
    (void)title;
    (void)path;
    return false;
#else
    // Called on the main thread before the game starts (no SDL window yet).
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.message = ns(title);
        panel.prompt = @"Choose";
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK || !panel.URL) return false;
        path = panel.URL.path.UTF8String;
        return true;
    }
#endif
}

}  // namespace platform
