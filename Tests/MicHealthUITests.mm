#import "MicHealthUI.h"
#include <cstdio>

int main() {
    @autoreleasepool {
        unsigned failures = 0;
        auto check = [&](BOOL passed, const char *message) {
            if (!passed) { ++failures; fprintf(stderr, "FAIL: %s\n", message); }
        };
        const double now = NSDate.date.timeIntervalSince1970;
        NSDictionary *warning = @{@"message": @"입력 지연 경고", @"updatedAt": @(now)};
        check([MSMicHealthSummary(warning) isEqual:@"입력 지연 경고"], "show current warning");
        check([MSMicHealthSummary(@{@"message": @"정상", @"updatedAt": @(now - 20)})
            containsString:@"갱신 중단"], "never present a stopped engine's last state as current");
        check(MSMicHealthSummary(@{}).length > 0, "missing status still has a readable summary");
        NSMutableArray *events = [NSMutableArray array];
        for (unsigned i = 0; i < 70; ++i) {
            [events addObject:@{@"time": @(now - 70 + i),
                @"level": i == 69 ? @"error" : (i == 68 ? @"warning" : @"info"),
                @"message": [NSString stringWithFormat:@"event-%02u", i]}];
        }
        NSString *log = MSMicHealthLogText(@{@"events": events});
        NSArray<NSString *> *lines = [log componentsSeparatedByString:@"\n\n"];
        check(lines.count == 64, "bound the displayed history");
        check([lines.firstObject containsString:@"[오류] event-69"], "show newest error first");
        check([lines[1] containsString:@"[경고] event-68"], "distinguish warnings from errors");
        check([lines.lastObject containsString:@"[정보] event-06"], "preserve chronological history");
        check(![log containsString:@"event-05"], "do not show older excess events");
        check(MSMicHealthLogText(@{}).length > 0, "explain the empty history");
        if (!failures) puts("All microphone health UI tests passed");
        return failures ? 1 : 0;
    }
}
