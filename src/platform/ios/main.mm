// The iOS / iPadOS and visionOS app. Free play is forced (there is no coin slot).
//
// On Apple Vision Pro it runs in a floating window (16:10, resized
// uniformly, the 4:3 picture between black bars). Look and pinch is a tap.
// Controller presses go only to the Game Controller framework
// (GCEventInteraction); a PlayStation VR2 Sense trigger fires at the
// crosshair its thumbsticks move (platform/apple/controllers.hpp). In case
// the system still sends a pad's press as a pinch, a touch within
// kPinchWindow of a gamepad press is dropped.
//
// Touch: the START button (bottom right) starts a game. A tap on the
// picture shoots where it lands; a tap anywhere outside it (the black bars)
// or a two-finger tap is an off-screen shot: reload and remodulate. Game
// controllers work as on the Mac (platform/apple/controllers). Using one
// shows its crosshair and hides the START button; touching the screen hides
// the crosshair and brings the button back.
//
// The game files go in the app's Documents folder (the Files app, or Finder
// with the device connected): the folder holding trek, stvsnd, stvserv and
// stv/, either Documents itself or one folder inside it.
#import <AVFAudio/AVFAudio.h>
#import <GameController/GameController.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "app/session.hpp"
#include "gpu/renderer.hpp"
#include "platform/apple/audio.hpp"
#include "platform/apple/controllers.hpp"
#include "platform/apple/game_host.hpp"

using namespace voy;

namespace {

bool has_game(const std::string& dir) {
    struct stat st;
    return ::stat((dir + "/trek").c_str(), &st) == 0 && ::stat((dir + "/stv").c_str(), &st) == 0;
}

std::string find_content() {
    NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!docs) return {};
    const std::string root = docs.UTF8String;
    if (has_game(root)) return root;
    for (NSString* name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:docs error:nil]) {
        const std::string sub = root + "/" + name.UTF8String;
        if (has_game(sub)) return sub;
    }
    return {};
}

#if TARGET_OS_VISION
// visionOS: touches wait this long for a gamepad press that would make them
// the pad's own pinch, which the pad has already handled.
constexpr double kTouchDelay = 0.05;
constexpr double kPinchWindow = 0.15;
const CGSize kWindowSize = {1440, 900};
#endif

struct PendingTouch {
    double t;
    float gx, gy;
    bool inside;
};

}  // namespace

@interface GameViewController : UIViewController <MTKViewDelegate>
@end

@implementation GameViewController {
    std::unique_ptr<platform::GameHost> host;
    std::unique_ptr<gpu::Renderer> renderer;
    std::unique_ptr<platform::AudioOut> audio;
    platform::Controllers pads;
    float rect[4];
    int start_hold;  // frames left holding Start after the START button
    std::chrono::steady_clock::time_point last;
    UILabel* message;
    UIButton* start;
    bool pad_mode;  // the controller was used last: crosshair shown, START hidden
    std::vector<PendingTouch> pending;  // visionOS: touches waiting out kTouchDelay
}

#if !TARGET_OS_VISION
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
#endif

- (void)loadView {
#if TARGET_OS_VISION
    const CGRect frame = {CGPointZero, kWindowSize};
#else
    const CGRect frame = UIScreen.mainScreen.bounds;
#endif
    MTKView* v = [[MTKView alloc] initWithFrame:frame device:MTLCreateSystemDefaultDevice()];
    v.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    v.preferredFramesPerSecond = 60;
    v.delegate = self;
    v.multipleTouchEnabled = YES;
    v.backgroundColor = UIColor.blackColor;
#if TARGET_OS_VISION
    // Without this, visionOS turns controller presses into pinches at the
    // gaze point and the Game Controller framework reads nothing. Delivery
    // must be exclusive: asking for the view as well (receivesEventsInView)
    // left both empty on visionOS 27.
    GCEventInteraction* gc = [GCEventInteraction new];
    gc.handledEventTypes = GCUIEventTypeGamepad;
    [v addInteraction:gc];
#endif
    self.view = v;
}

- (void)showMessage:(NSString*)text {
    message = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 40, 40)];
    message.text = text;
    message.numberOfLines = 0;
    message.textAlignment = NSTextAlignmentCenter;
    message.textColor = UIColor.whiteColor;
    message.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:message];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (host && self.view.window) pads.attach_window((__bridge void*)self.view.window);
}

- (void)viewDidLoad {
    [super viewDidLoad];
    const std::string content = find_content();
    if (content.empty()) {
        [self showMessage:@"Game files not found.\n\nCopy your Star Trek: Voyager arcade game folder (the one holding "
                          @"trek, stvsnd, stvserv and stv) into this app's folder with the Files app or Finder, "
                          @"then open the app again."];
        return;
    }
    const std::string bad = app::verify_content(content);
    if (!bad.empty()) {
        [self showMessage:[NSString stringWithFormat:@"These game files are not the e4 build this port needs:\n\n%s",
                                                     bad.c_str()]];
        return;
    }
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:nil];
    [AVAudioSession.sharedInstance setActive:YES error:nil];
    host::Config cfg;
    cfg.content = content;
    cfg.container = app::default_container();
    cfg.free_play = true;
    cfg.stdout_echo = getenv("VOY_STDOUT") != nullptr;
    MTKView* v = (MTKView*)self.view;
    gpu::RendererOptions ro;
    ro.scale = 2;
    renderer = gpu::Renderer::create((__bridge void*)v.device, ro);
    host = std::make_unique<platform::GameHost>(cfg);
    audio = std::make_unique<platform::AudioOut>(host->machine());
    audio->start();
    // Where a Sense controller points (0..1 of the window, which the view fills) in game coordinates.
    __weak GameViewController* weak = self;
    pads.set_window_to_game([weak](float fx, float fy, float* gx, float* gy) {
        GameViewController* me = weak;
        if (!me) return false;
        const CGSize size = me.view.bounds.size;
        const CGFloat s = me.view.contentScaleFactor;
        return platform::to_game(me->rect, float(fx * size.width * s), float(fy * size.height * s), gx, gy);
    });
    last = std::chrono::steady_clock::now();
    host->start();

    start = [UIButton buttonWithType:UIButtonTypeSystem];
    [start setTitle:@"START" forState:UIControlStateNormal];
    start.titleLabel.font = [UIFont boldSystemFontOfSize:18];
    [start setTitleColor:[UIColor colorWithWhite:1 alpha:0.85] forState:UIControlStateNormal];
    start.backgroundColor = [UIColor colorWithRed:0.1 green:0.3 blue:0.7 alpha:0.45];
    start.layer.cornerRadius = 36;
    start.layer.borderWidth = 2;
    start.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor;
    [start addTarget:self action:@selector(startPressed) forControlEvents:UIControlEventTouchDown];
    [self.view addSubview:start];
    [self placeStart];
}

// The START button goes in the black bar right of the 4:3 picture when the
// bar (inside the safe area) is wide enough, else over the picture's corner.
- (void)placeStart {
    if (!start) return;
    const CGRect b = self.view.bounds;
    const UIEdgeInsets safe = self.view.safeAreaInsets;
    const CGFloat pic_w = MIN(b.size.width, b.size.height * 4 / 3);
    const CGFloat pic_right = (b.size.width + pic_w) / 2;
    const CGFloat bar = b.size.width - safe.right - pic_right;
    const CGFloat h = 72, bottom = b.size.height - MAX(safe.bottom, 12.0) - h;
    if (bar >= 80 + 8) {
        const CGFloat w = MIN(110.0, bar - 8);
        start.frame = CGRectMake(pic_right + (bar - w) / 2, bottom, w, h);
    } else {
        start.frame = CGRectMake(b.size.width - safe.right - 12 - 110, bottom, 110, h);
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self placeStart];
}

- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    [self placeStart];
}

- (void)startPressed {
    platform::input_log("START button");
    if (!host) return;
    host->machine().input().set_button(host::Input::Start1, true);
    start_hold = 8;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* t in touches) {
        const CGPoint p = [t locationInView:self.view];
        platform::input_log("touch began at %.0f, %.0f (type %ld, %lu in event)", p.x, p.y, (long)t.type,
                            (unsigned long)event.allTouches.count);
    }
    if (!host) return;
    const double now = platform::Controllers::now_seconds();
    if (event.allTouches.count >= 2) {  // two fingers (two hands): reload
        pending.push_back({now, 0, 0, false});
    } else {
        const CGFloat s = self.view.contentScaleFactor;
        for (UITouch* t in touches) {
            const CGPoint p = [t locationInView:self.view];
            PendingTouch pt{now, 0, 0, false};
            pt.inside = platform::to_game(rect, float(p.x * s), float(p.y * s), &pt.gx, &pt.gy);
            pending.push_back(pt);
        }
    }
#if !TARGET_OS_VISION
    [self fireTouches:now];
#endif
}

// Fires the touches that are due: on iOS at once, on visionOS after
// kTouchDelay unless a gamepad press shows them to be its pinch.
- (void)fireTouches:(double)now {
    std::size_t kept = 0;
    for (const PendingTouch& t : pending) {
#if TARGET_OS_VISION
        if (now - t.t < kTouchDelay) {
            pending[kept++] = t;
            continue;
        }
        if (pads.pad_press_near(t.t, kPinchWindow)) {
            platform::input_log("touch dropped: a gamepad press came with it");
            continue;
        }
#endif
        pad_mode = false;  // touch play: START back, crosshair hidden
        platform::input_log("touch shot at %.0f, %.0f (inside %d)", t.gx, t.gy, t.inside);
        host->pointer_shot(0, t.gx, t.gy, t.inside);
    }
    pending.resize(kept);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView*)view {
    if (!host) return;
    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (dt > 0.1) dt = 0.1;
    if (start_hold > 0 && --start_hold == 0) host->machine().input().set_button(host::Input::Start1, false);
    pads.poll(host->machine(), dt);
    if (!pending.empty()) [self fireTouches:platform::Controllers::now_seconds()];
    if (pads.take_activity()) pad_mode = true;  // controller play: Start is on the pad
    start.hidden = pad_mode;
    if (auto f = host->take_new()) renderer->draw(*f);
    gpu::Renderer::Cursor cur[2];
    pads.cursors(cur);
    if (!pad_mode) cur[0].visible = cur[1].visible = false;
    renderer->set_cursors(cur, 2);
    id<CAMetalDrawable> d = view.currentDrawable;
    if (d) renderer->present((__bridge void*)d, int(view.drawableSize.width), int(view.drawableSize.height), rect);
}

- (void)shutdown {
    if (host) host->stop();
    if (audio) audio->stop();
}
@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    UIWindowScene* ws = (UIWindowScene*)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:ws];
    self.window.rootViewController = [GameViewController new];
    [self.window makeKeyAndVisible];
#if TARGET_OS_VISION
    // A floating window, resized uniformly so the bars beside the picture
    // keep room for START (right) and a reload pinch (either side).
    UIWindowSceneGeometryPreferencesVision* g = [[UIWindowSceneGeometryPreferencesVision alloc] initWithSize:kWindowSize];
    g.minimumSize = CGSizeMake(kWindowSize.width * 0.5, kWindowSize.height * 0.5);
    g.maximumSize = CGSizeMake(kWindowSize.width * 2.5, kWindowSize.height * 2.5);
    g.resizingRestrictions = UIWindowSceneResizingRestrictionsUniform;
    [ws requestGeometryUpdateWithPreferences:g errorHandler:nil];
#endif
}
// Controllers report only to the active app: logged to tell the two apart.
- (void)sceneDidBecomeActive:(UIScene*)scene { platform::input_log("scene active"); }
- (void)sceneWillResignActive:(UIScene*)scene { platform::input_log("scene inactive"); }
- (void)sceneDidEnterBackground:(UIScene*)scene { platform::input_log("scene background"); }
- (void)sceneWillEnterForeground:(UIScene*)scene { platform::input_log("scene foreground"); }
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options { return YES; }
- (UISceneConfiguration*)application:(UIApplication*)app
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = SceneDelegate.class;
    return c;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
