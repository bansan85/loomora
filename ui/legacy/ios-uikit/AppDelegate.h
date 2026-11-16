// Application delegate of the iOS Legacy UI: creates the window and its only screen.
#import <UIKit/UIKit.h>

@interface AppDelegate : UIResponder <UIApplicationDelegate> {
    UIWindow *window;
}

@property(nonatomic, retain) UIWindow *window;

@end
