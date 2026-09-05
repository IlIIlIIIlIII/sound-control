#import "MicHealthUI.h"

NSString *MSMicHealthPath(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:
        @"Library/Application Support/MacTools/mic-health.json"];
}

NSDictionary *MSLoadMicHealth(void) {
    NSData *data = [NSData dataWithContentsOfFile:MSMicHealthPath()];
    if (!data || data.length > 256 * 1024) return @{};
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [value isKindOfClass:NSDictionary.class] ? value : @{};
}

NSString *MSMicHealthSummary(NSDictionary *health) {
    NSString *message = [health[@"message"] isKindOfClass:NSString.class] ? health[@"message"] : nil;
    const double updated = [health[@"updatedAt"] doubleValue];
    if (updated > 0 && NSDate.date.timeIntervalSince1970 - updated > 10)
        return @"마이크 상태 갱신 중단 · 아래 기록은 이전 실행의 기록입니다.";
    return message.length ? message : @"자동 복구: 상태를 기다리는 중입니다.";
}

NSString *MSMicHealthEventText(NSDictionary *event) {
    if (![event isKindOfClass:NSDictionary.class]) return @"";
    NSDateFormatter *format = [NSDateFormatter new];
    format.dateFormat = @"MM/dd HH:mm:ss";
    NSString *date = [format stringFromDate:[NSDate dateWithTimeIntervalSince1970:
        [event[@"time"] doubleValue]]];
    NSString *level = [event[@"level"] isEqual:@"warning"] ? @"경고" :
        ([event[@"level"] isEqual:@"error"] ? @"오류" : @"정보");
    NSString *message = [event[@"message"] isKindOfClass:NSString.class] ? event[@"message"] : @"";
    return [NSString stringWithFormat:@"%@ [%@] %@", date, level, message];
}

NSString *MSMicHealthLogText(NSDictionary *health) {
    NSArray *events = [health[@"events"] isKindOfClass:NSArray.class] ? health[@"events"] : @[];
    if (!events.count) return @"기록된 경고나 복구 작업이 없습니다.";
    NSMutableArray *lines = [NSMutableArray array];
    for (id event in events.reverseObjectEnumerator) {
        NSString *line = MSMicHealthEventText(event);
        if (line.length) [lines addObject:line];
        if (lines.count == 64) break;
    }
    return [lines componentsJoinedByString:@"\n\n"];
}
