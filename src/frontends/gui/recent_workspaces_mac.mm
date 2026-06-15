// macOS system recent-documents integration for RecentWorkspaces (S6.9 Stage C).
//
// Objective-C++ shim kept out of the header so the cross-platform
// RecentWorkspaces class stays pure C++. Declared in recent_workspaces.h.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <string>

namespace locus {

void note_recent_document_macos(const std::string& path)
{
    @autoreleasepool {
        NSString* p = [NSString stringWithUTF8String:path.c_str()];
        if (!p) return;
        // A workspace is a directory; flag it so the URL is built correctly.
        NSURL* url = [NSURL fileURLWithPath:p isDirectory:YES];
        if (!url) return;
        [[NSDocumentController sharedDocumentController]
            noteNewRecentDocumentURL:url];
    }
}

} // namespace locus
