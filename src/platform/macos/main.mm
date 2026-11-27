// The Mac app: a window showing the cabinet's screen, letterboxed to 4:3.
//
// Mouse: aim and click to shoot; right click (or a click outside the picture)
// reloads / remodulates. Keys: 5 / 6 coin, 1 / 2 start, F2 the service menu
// (the cabinet's test switch), Ctrl-Cmd-F full screen, Cmd-Q quit. Game
// controllers: see platform/apple/controllers.hpp.
//
// The game folder comes from --content <folder>, VOY_CONTENT, or the folder
// chosen the first time (kept in the app's defaults as ContentFolder).
#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>

#include <chrono>
#include <memory>
#include <string>

#include "app/session.hpp"
#include "gpu/renderer.hpp"
#include "host/fail.hpp"
#include "platform/apple/audio.hpp"
#include "platform/apple/controllers.hpp"
#include "platform/apple/game_host.hpp"

using namespace voy;

@interface GameView : MTKView <MTKViewDelegate>
@end

@implementation GameView {
@public
    std::unique_ptr<platform::GameHost> host;
    std::unique_ptr<gpu::Renderer> renderer;
    std::unique_ptr<platform::AudioOut> audio;
    platform::Controllers pads;
    float rect[4];
    bool mouse_down, right_down;
    double repeat;
    NSPoint last_mouse;
    std::chrono::steady_clock::time_point last_time;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView*)view {
    if (!host) return;
    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - last_time).count();
    last_time = now;
    if (dt > 0.1) dt = 0.1;
    pads.poll(host->machine(), dt);
    // Held mouse button: the trigger repeats.
    if (mouse_down) {
        repeat -= dt;
        if (repeat <= 0) {
            [self shootAt:last_mouse gun:0];
            repeat = 0.12;
        }
    }
    if (auto f = host->take_new()) {
        renderer->draw(*f);
        // VOY_CAPTURE=<prefix>: every 300th game frame as <prefix>_<frame>.png (debugging).
        static const char* capture = getenv("VOY_CAPTURE");
        if (capture && f->index % 300 == 0) {
            std::vector<std::uint8_t> px;
            int w, h;
            if (renderer->read_rgba(px, &w, &h)) {
                char name[32];
                snprintf(name, sizeof name, "_%06llu.png", (unsigned long long)f->index);
                gpu::write_png(std::string(capture) + name, px, w, h);
            }
        }
    }
    gpu::Renderer::Cursor cur[2];
    pads.cursors(cur);
    renderer->set_cursors(cur, 2);
    id<CAMetalDrawable> d = view.currentDrawable;
    if (d) renderer->present((__bridge void*)d, int(view.drawableSize.width), int(view.drawableSize.height), rect);
    if (!host->running()) [NSApp terminate:nil];
}

- (void)shootAt:(NSPoint)p gun:(int)gun {
    const CGFloat s = self.window.backingScaleFactor;
    const float px = float(p.x * s), py = float((self.bounds.size.height - p.y) * s);
    float gx, gy;
    const bool inside = platform::to_game(rect, px, py, &gx, &gy);
    host->pointer_shot(gun, gx, gy, inside);
}

- (void)mouseDown:(NSEvent*)e {
    last_mouse = [self convertPoint:e.locationInWindow fromView:nil];
    [self shootAt:last_mouse gun:0];
    mouse_down = true;
    repeat = 0.18;
}
- (void)mouseDragged:(NSEvent*)e { last_mouse = [self convertPoint:e.locationInWindow fromView:nil]; }
- (void)mouseUp:(NSEvent*)e { mouse_down = false; }
- (void)rightMouseDown:(NSEvent*)e { host->machine().input().shot(0, -1, -1); }

- (void)setKey:(NSEvent*)e down:(BOOL)down {
    auto& in = host->machine().input();
    const unsigned short code = e.keyCode;
    NSString* ch = e.charactersIgnoringModifiers.lowercaseString;
    if (code == 120) {  // F2: the test switch → service menu
        in.set_button(host::Input::Test, down);
        return;
    }
    if (ch.length == 0) return;
    switch ([ch characterAtIndex:0]) {
    case '5': if (down && !e.isARepeat) in.coin(0); break;
    case '6': if (down && !e.isARepeat) in.coin(1); break;
    case '1': case '\r': in.set_button(host::Input::Start1, down); break;
    case '2': in.set_button(host::Input::Start2, down); break;
    case ' ': if (down && !e.isARepeat) in.shot(0, -1, -1); break;  // reload / remodulate
    case 'p': case 'o': if (down && !e.isARepeat) in.key([ch characterAtIndex:0]); break;  // the game's pause keys
    default: break;
    }
}
- (void)keyDown:(NSEvent*)e { [self setKey:e down:YES]; }
- (void)keyUp:(NSEvent*)e { [self setKey:e down:NO]; }

- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:NSCursor.crosshairCursor]; }
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
    NSWindow* window;
    GameView* view;
}

static std::string choose_content() {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 0; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:@"--content"]) return args[i + 1].UTF8String;
    if (const char* e = getenv("VOY_CONTENT")) return e;
    NSString* saved = [NSUserDefaults.standardUserDefaults stringForKey:@"ContentFolder"];
    if (saved) return saved.UTF8String;
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.message = @"Choose your Star Trek: Voyager arcade game folder (the one holding trek, stvsnd and stv/).";
    p.prompt = @"Use This Folder";
    if ([p runModal] != NSModalResponseOK) return {};
    NSString* path = p.URL.path;
    [NSUserDefaults.standardUserDefaults setObject:path forKey:@"ContentFolder"];
    return path.UTF8String;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    const std::string content = choose_content();
    if (content.empty()) {
        [NSApp terminate:nil];
        return;
    }
    const std::string bad = app::verify_content(content);
    if (!bad.empty()) {
        NSAlert* a = [NSAlert new];
        a.messageText = @"This is not the Star Trek: Voyager arcade game folder this port needs (build e4).";
        a.informativeText = [NSString stringWithUTF8String:bad.c_str()];
        [a runModal];
        [NSUserDefaults.standardUserDefaults removeObjectForKey:@"ContentFolder"];
        [NSApp terminate:nil];
        return;
    }
    host::Config cfg;
    cfg.content = content;
    cfg.container = app::default_container();
    cfg.stdout_echo = getenv("VOY_STDOUT") != nullptr;
    cfg.free_play = [NSUserDefaults.standardUserDefaults boolForKey:@"FreePlay"] || getenv("VOY_FREE_PLAY");
    if (const char* s = getenv("VOY_SCRIPT")) cfg.input_script = s;

    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1024, 768)
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    window.title = @"Voyager Arcade";
    window.contentAspectRatio = NSMakeSize(4, 3);
    window.delegate = self;
    window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    view = [[GameView alloc] initWithFrame:window.contentView.bounds device:MTLCreateSystemDefaultDevice()];
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.preferredFramesPerSecond = 60;
    view.delegate = view;
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    gpu::RendererOptions ro;
    ro.scale = 3;
    view->renderer = gpu::Renderer::create((__bridge void*)view.device, ro);
    view->host = std::make_unique<platform::GameHost>(cfg);
    view->audio = std::make_unique<platform::AudioOut>(view->host->machine());
    view->audio->start();
    view->last_time = std::chrono::steady_clock::now();
    window.contentView = view;
    [window center];
    [window makeKeyAndOrderFront:nil];
    [window makeFirstResponder:view];
    view->host->start();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { return YES; }

- (void)applicationWillTerminate:(NSNotification*)n {
    if (view && view->host) {
        view->host->stop();
        if (view->audio) view->audio->stop();
    }
}
@end

static void build_menu() {
    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu* appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit Voyager Arcade" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSMenuItem* viewItem = [NSMenuItem new];
    [bar addItem:viewItem];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem* fs = [viewMenu addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    viewItem.submenu = viewMenu;
    NSApp.mainMenu = bar;
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        build_menu();
        AppDelegate* d = [AppDelegate new];
        app.delegate = d;
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
