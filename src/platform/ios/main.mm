// The iOS / iPadOS app. Free play is forced (there is no coin slot).
//
// Touch: when player 1 is not in a game, a tap starts one; in a game, a tap
// shoots where it lands. The RELOAD button (or a two-finger tap, or a tap on
// the black bars outside the picture) is an off-screen shot: reload and
// remodulate. Game controllers work as on the Mac (platform/apple/controllers).
//
// The game files go in the app's Documents folder (the Files app, or Finder
// with the device connected): the folder holding trek, stvsnd, stvserv and
// stv/, either Documents itself or one folder inside it.
#import <AVFAudio/AVFAudio.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <chrono>
#include <memory>
#include <string>
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

}  // namespace

@interface GameViewController : UIViewController <MTKViewDelegate>
@end

@implementation GameViewController {
    std::unique_ptr<platform::GameHost> host;
    std::unique_ptr<gpu::Renderer> renderer;
    std::unique_ptr<platform::AudioOut> audio;
    platform::Controllers pads;
    float rect[4];
    int start_hold;  // frames left holding Start for a tap-to-start
    std::chrono::steady_clock::time_point last;
    UILabel* message;
    UIButton* reload;
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }

- (void)loadView {
    MTKView* v = [[MTKView alloc] initWithFrame:UIScreen.mainScreen.bounds device:MTLCreateSystemDefaultDevice()];
    v.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    v.preferredFramesPerSecond = 60;
    v.delegate = self;
    v.multipleTouchEnabled = YES;
    v.backgroundColor = UIColor.blackColor;
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
    last = std::chrono::steady_clock::now();
    host->start();

    reload = [UIButton buttonWithType:UIButtonTypeSystem];
    [reload setTitle:@"RELOAD" forState:UIControlStateNormal];
    reload.titleLabel.font = [UIFont boldSystemFontOfSize:18];
    [reload setTitleColor:[UIColor colorWithWhite:1 alpha:0.85] forState:UIControlStateNormal];
    reload.backgroundColor = [UIColor colorWithRed:0.1 green:0.3 blue:0.7 alpha:0.45];
    reload.layer.cornerRadius = 36;
    reload.layer.borderWidth = 2;
    reload.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor;
    reload.translatesAutoresizingMaskIntoConstraints = NO;
    [reload addTarget:self action:@selector(reloadPressed) forControlEvents:UIControlEventTouchDown];
    [self.view addSubview:reload];
    [NSLayoutConstraint activateConstraints:@[
        [reload.widthAnchor constraintEqualToConstant:110], [reload.heightAnchor constraintEqualToConstant:72],
        [reload.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-12],
        [reload.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-12],
    ]];
}

- (void)reloadPressed {
    if (host) host->machine().input().shot(0, -1, -1);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!host) return;
    if (event.allTouches.count >= 2) {  // two fingers: reload
        host->machine().input().shot(0, -1, -1);
        return;
    }
    if (!host->machine().player_playing(0)) {
        host->machine().input().set_button(host::Input::Start1, true);
        start_hold = 8;
        return;
    }
    const CGFloat s = self.view.contentScaleFactor;
    for (UITouch* t in touches) {
        const CGPoint p = [t locationInView:self.view];
        float gx, gy;
        const bool inside = platform::to_game(rect, float(p.x * s), float(p.y * s), &gx, &gy);
        host->pointer_shot(0, gx, gy, inside);
    }
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
    if (auto f = host->take_new()) renderer->draw(*f);
    gpu::Renderer::Cursor cur[2];
    pads.cursors(cur);
    renderer->set_cursors(cur, 2);
    reload.hidden = pads.any_connected();
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
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [GameViewController new];
    [self.window makeKeyAndVisible];
}
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
