#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

extern NSString *const MTDisplayReinitializeNotification;
extern NSString *const MTDisplaySwapNotification;

@interface MTDisplayController : NSObject

- (void)start;
- (void)shutdown;
- (NSDictionary *)diagnosticSnapshot;

@end

NS_ASSUME_NONNULL_END
