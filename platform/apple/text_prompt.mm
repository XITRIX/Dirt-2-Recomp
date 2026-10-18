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

void show(void* userdata) {
    auto* p = static_cast<Prompt*>(userdata);
#if TARGET_OS_IPHONE
    UIViewController* root = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class])
            for (UIWindow* w in static_cast<UIWindowScene*>(scene).windows)
                if (w.isKeyWindow) root = w.rootViewController;
    while (root.presentedViewController) root = root.presentedViewController;
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
               dispatch_semaphore_signal(p->done);
           }]];
    UIAlertController* __weak weak = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
               p->result = weak.textFields.firstObject.text.UTF8String ?: "";
               p->ok = true;
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

void set_app_icon(const std::string& path) {
#if !TARGET_OS_IPHONE
    if (NSImage* icon = [[NSImage alloc] initWithContentsOfFile:ns(path)]) NSApp.applicationIconImage = icon;
#else
    (void)path;  // the app bundle has its own icon
#endif
}

}  // namespace platform
