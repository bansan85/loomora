// Application delegate of the iOS Legacy UI (manual retain/release, no ARC).
#import "AppDelegate.h"

#import "ViewController.h"

@implementation AppDelegate

@synthesize window;

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    ViewController *controller = [[[ViewController alloc] init] autorelease];
    UIWindow *mainWindow =
        [[[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]] autorelease];
    mainWindow.rootViewController = controller;
    self.window = mainWindow;
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)dealloc {
    [window release];
    [super dealloc];
}

@end
