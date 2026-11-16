// The only screen of the iOS Legacy UI: a UITextField pre-filled with "Coucou" (limited to 1,000
// characters), a UILabel with the greeting of the backend, a banner for the end-of-life warning.
#import <UIKit/UIKit.h>

@interface ViewController : UIViewController <UITextFieldDelegate> {
    UITextField *inputField;
    UIScrollView *greetingScroll;
    UILabel *greetingLabel;
    UILabel *eolLabel;
    BOOL abiMatches;
}

@end
