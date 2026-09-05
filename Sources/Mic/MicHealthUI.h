#import <Foundation/Foundation.h>

NSString *MSMicHealthPath(void);
NSDictionary *MSLoadMicHealth(void);
NSString *MSMicHealthSummary(NSDictionary *health);
NSString *MSMicHealthEventText(NSDictionary *event);
NSString *MSMicHealthLogText(NSDictionary *health);
