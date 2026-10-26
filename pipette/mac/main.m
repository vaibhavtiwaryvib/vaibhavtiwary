// Pipette for macOS — a small native window around the Pipette web app.
// © 2026 VITIRAN (Vaibhav Tiwary). MIT licence.
//
// Everything statistical lives in Pipette.html (the same file as the browser version).
// This file only gives it a real Mac window, menus, Save/Open dialogs, clipboard and autosave.
// Build: see build.sh (compiled with clang against the macOS SDK; no Xcode project needed).

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

static NSString *const kVersion = @"1.2";

@interface PipetteApp : NSObject <NSApplicationDelegate, NSWindowDelegate, WKScriptMessageHandlerWithReply, WKUIDelegate, WKNavigationDelegate>
@property (strong) NSWindow *window;
@property (strong) WKWebView *web;
@property (strong) NSMutableArray<NSURL *> *pendingOpen;
@property (assign) BOOL pageReady;
@property (assign) BOOL terminating;
@end

@implementation PipetteApp

#pragma mark - files

- (NSURL *)supportDir {
    NSURL *base = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *dir = [base URLByAppendingPathComponent:@"Pipette" isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}
- (NSURL *)autosaveURL { return [[self supportDir] URLByAppendingPathComponent:@"autosave.pipette"]; }

- (void)writeAutosave:(NSString *)json {
    if (![json isKindOfClass:[NSString class]] || json.length < 2) return;
    NSURL *url = [self autosaveURL];
    // keep one previous copy, just in case
    NSURL *bak = [[self supportDir] URLByAppendingPathComponent:@"autosave-previous.pipette"];
    [[NSFileManager defaultManager] removeItemAtURL:bak error:nil];
    [[NSFileManager defaultManager] copyItemAtURL:url toURL:bak error:nil];
    [json writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

#pragma mark - launch

- (void)applicationWillFinishLaunching:(NSNotification *)n {
    self.pendingOpen = [NSMutableArray array];
    [self buildMenus];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    WKUserContentController *uc = [WKUserContentController new];
    [uc addScriptMessageHandlerWithReply:self contentWorld:[WKContentWorld pageWorld] name:@"pipette"];

    // Hand the last autosaved project to the page before it boots.
    NSData *saved = [NSData dataWithContentsOfURL:[self autosaveURL]];
    if (saved.length > 2) {
        NSString *b64 = [saved base64EncodedStringWithOptions:0];
        NSString *js = [NSString stringWithFormat:
            @"try { window.__pipetteAutosave = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob('%@'), c => c.charCodeAt(0)))); } catch (e) { }", b64];
        [uc addUserScript:[[WKUserScript alloc] initWithSource:js injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
    }

    WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
    cfg.userContentController = uc;
    @try { [cfg.preferences setValue:@YES forKey:@"developerExtrasEnabled"]; } @catch (id e) {}

    NSRect frame = NSMakeRect(0, 0, 1280, 820);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Pipette";
    self.window.minSize = NSMakeSize(900, 560);
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    self.window.tabbingMode = NSWindowTabbingModeDisallowed;
    [self.window center];
    [self.window setFrameAutosaveName:@"PipetteMainWindow"];

    self.web = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
    self.web.UIDelegate = self;
    self.web.navigationDelegate = self;
    self.web.allowsMagnification = YES;
    self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.window.contentView = self.web;

    NSURL *html = [[NSBundle mainBundle] URLForResource:@"Pipette" withExtension:@"html"];
    [self.web loadFileURL:html allowingReadAccessToURL:[html URLByDeletingLastPathComponent]];

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app { return YES; }

// Make sure the very latest edits are on disk before quitting.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    if (!self.web || !self.pageReady || self.terminating) return NSTerminateNow;
    self.terminating = YES;
    __block BOOL done = NO;
    [self.web evaluateJavaScript:@"window.Pipette ? Pipette.command('flush') : ''" completionHandler:^(id result, NSError *err) {
        if (done) return; done = YES;
        if ([result isKindOfClass:[NSString class]]) [self writeAutosave:result];
        [NSApp replyToApplicationShouldTerminate:YES];
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (done) return; done = YES; [NSApp replyToApplicationShouldTerminate:YES];
    });
    return NSTerminateLater;
}

// Double-clicking a .pipette file in Finder, or dropping it on the Dock icon.
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    [self.pendingOpen addObjectsFromArray:urls];
    if (self.pageReady) [self flushPendingOpen];
    if (!self.window.visible) [self.window makeKeyAndOrderFront:nil];
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)flag {
    if (!flag) [self.window makeKeyAndOrderFront:nil];
    return YES;
}

- (void)flushPendingOpen {
    NSArray *urls = [self.pendingOpen copy];
    [self.pendingOpen removeAllObjects];
    for (NSURL *url in urls) [self openProjectAtURL:url];
}
- (void)openProjectAtURL:(NSURL *)url {
    NSString *text = [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:nil];
    if (!text) { [self alert:@"This file could not be opened." info:url.lastPathComponent]; return; }
    [self.web callAsyncJavaScript:@"Pipette.openText(name, text)"
                        arguments:@{ @"name": url.lastPathComponent ?: @"Project", @"text": text }
                          inFrame:nil inContentWorld:[WKContentWorld pageWorld] completionHandler:nil];
    [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:url];
}

#pragma mark - page → Mac bridge

- (void)userContentController:(WKUserContentController *)uc didReceiveScriptMessage:(WKScriptMessage *)message
                 replyHandler:(void (^)(id _Nullable, NSString * _Nullable))reply {
    NSDictionary *d = [message.body isKindOfClass:[NSDictionary class]] ? message.body : @{};
    NSString *cmd = d[@"cmd"];

    if ([cmd isEqualToString:@"autosave"]) {
        [self writeAutosave:d[@"data"]];
        reply(@"ok", nil);
    } else if ([cmd isEqualToString:@"save"]) {
        NSData *data = [[NSData alloc] initWithBase64EncodedString:d[@"data"] ?: @"" options:0];
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = d[@"name"] ?: @"Untitled";
        panel.canCreateDirectories = YES;
        panel.extensionHidden = NO;
        NSString *ext = d[@"ext"];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (ext.length) panel.allowedFileTypes = @[ext];
#pragma clang diagnostic pop
        [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
            if (r != NSModalResponseOK) { reply(@"cancel", nil); return; }
            BOOL ok = data && [data writeToURL:panel.URL atomically:YES];
            if (ok && [ext isEqualToString:@"pipette"]) [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:panel.URL];
            reply(ok ? @"ok" : @"error", nil);
        }];
    } else if ([cmd isEqualToString:@"open"]) {
        [self showOpenPanel];
        reply(@"ok", nil);
    } else if ([cmd isEqualToString:@"copyImage"]) {
        NSData *png = [[NSData alloc] initWithBase64EncodedString:d[@"data"] ?: @"" options:0];
        NSImage *img = png ? [[NSImage alloc] initWithData:png] : nil;
        if (!img) { reply(@"error", nil); return; }
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb declareTypes:@[NSPasteboardTypePNG, NSPasteboardTypeTIFF] owner:nil];
        [pb setData:png forType:NSPasteboardTypePNG];
        [pb setData:img.TIFFRepresentation forType:NSPasteboardTypeTIFF];
        reply(@"ok", nil);
    } else if ([cmd isEqualToString:@"copyText"]) {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:d[@"data"] ?: @"" forType:NSPasteboardTypeString];
        reply(@"ok", nil);
    } else if ([cmd isEqualToString:@"title"]) {
        NSString *t = d[@"data"];
        self.window.title = t.length ? [NSString stringWithFormat:@"%@ — Pipette", t] : @"Pipette";
        reply(@"ok", nil);
    } else {
        reply(nil, @"unknown command");
    }
}

#pragma mark - navigation: keep the app in its window, send real links to the browser

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    self.pageReady = YES;
    [self flushPendingOpen];
}

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action
decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    NSURL *u = action.request.URL;
    NSString *s = u.scheme.lowercaseString;
    if ([s isEqualToString:@"about"] || [s isEqualToString:@"blob"] || [s isEqualToString:@"data"]) { decisionHandler(WKNavigationActionPolicyAllow); return; }
    if ([s isEqualToString:@"file"]) {
        NSURL *home = [[NSBundle mainBundle] URLForResource:@"Pipette" withExtension:@"html"];
        if (!self.pageReady || [u.path isEqualToString:home.path]) { decisionHandler(WKNavigationActionPolicyAllow); return; }
        // a file dropped onto the window: open projects, ignore everything else
        decisionHandler(WKNavigationActionPolicyCancel);
        if ([u.pathExtension.lowercaseString isEqualToString:@"pipette"] || [u.pathExtension.lowercaseString isEqualToString:@"json"]) [self openProjectAtURL:u];
        return;
    }
    decisionHandler(WKNavigationActionPolicyCancel);
    [[NSWorkspace sharedWorkspace] openURL:u];
}

- (WKWebView *)webView:(WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)c
   forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)f {
    if (action.request.URL) [[NSWorkspace sharedWorkspace] openURL:action.request.URL];
    return nil;
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    self.pageReady = NO;
    [webView reload];
}

#pragma mark - alert / confirm / prompt / file picker for the page

- (void)alert:(NSString *)msg info:(NSString *)info {
    NSAlert *a = [NSAlert new];
    a.messageText = msg; a.informativeText = info ?: @"";
    [a beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))done {
    NSAlert *a = [NSAlert new];
    a.messageText = message;
    [a addButtonWithTitle:@"OK"];
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { done(); }];
}

- (void)webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))done {
    NSAlert *a = [NSAlert new];
    a.messageText = message;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { done(r == NSAlertFirstButtonReturn); }];
}

- (void)webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt defaultText:(NSString *)def
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSString *))done {
    NSAlert *a = [NSAlert new];
    a.messageText = prompt;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
    field.stringValue = def ?: @"";
    a.accessoryView = field;
    [a.window setInitialFirstResponder:field];
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        done(r == NSAlertFirstButtonReturn ? field.stringValue : nil);
    }];
    dispatch_async(dispatch_get_main_queue(), ^{ [a.window makeFirstResponder:field]; [field selectText:nil]; });
}

- (void)webView:(WKWebView *)webView runOpenPanelWithParameters:(WKOpenPanelParameters *)params
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSArray<NSURL *> *))done {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseFiles = YES;
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = params.allowsMultipleSelection;
    [p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        if (r == NSModalResponseOK) {
            for (NSURL *u in p.URLs) [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:u];
            done(p.URLs);
        } else done(nil);
    }];
}

#pragma mark - menu commands → page

- (void)sendCommand:(NSString *)name {
    if (!self.pageReady) return;
    [self.web callAsyncJavaScript:@"return window.Pipette ? Pipette.command(name) : 'ok'"
                        arguments:@{ @"name": name } inFrame:nil inContentWorld:[WKContentWorld pageWorld]
                completionHandler:^(id result, NSError *err) {
        if ([result isEqual:@"native"]) {
            // the cursor is in a text field: let the field handle it (e.g. undo typing)
            SEL sel = [name isEqualToString:@"undo"] ? @selector(undo:) : [name isEqualToString:@"redo"] ? @selector(redo:) : NULL;
            if (sel) [NSApp sendAction:sel to:nil from:self];
        }
    }];
}
- (void)newTable:(id)s { [self sendCommand:@"newTable"]; }
- (void)newProject:(id)s { [self sendCommand:@"newProject"]; }
- (void)openProject:(id)s { [self showOpenPanel]; }
- (void)showOpenPanel {
    if (self.window.attachedSheet) return;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseFiles = YES; p.canChooseDirectories = NO; p.allowsMultipleSelection = NO;
    p.message = @"Choose a Pipette project (.pipette)";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    p.allowedFileTypes = @[@"pipette", @"json"];
#pragma clang diagnostic pop
    [p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        if (r == NSModalResponseOK && p.URL) [self openProjectAtURL:p.URL];
    }];
}
- (void)saveProject:(id)s { [self sendCommand:@"save"]; }
- (void)undoCmd:(id)s { [self sendCommand:@"undo"]; }
- (void)redoCmd:(id)s { [self sendCommand:@"redo"]; }
- (void)deleteRowsCmd:(id)s { [self sendCommand:@"deleteRows"]; }
- (void)cite:(id)s { [self sendCommand:@"cite"]; }
- (void)website:(id)s { [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"https://vaibhavtiwaryvib.github.io/vaibhavtiwary/pipette/"]]; }
- (void)zoomIn:(id)s { self.web.pageZoom = MIN(2.0, self.web.pageZoom + 0.1); }
- (void)zoomOut:(id)s { self.web.pageZoom = MAX(0.6, self.web.pageZoom - 0.1); }
- (void)zoomReset:(id)s { self.web.pageZoom = 1.0; }
- (void)about:(id)s {
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        @"ApplicationName": @"Pipette",
        @"ApplicationVersion": kVersion,
        @"Version": @"",
        @"Credits": [[NSAttributedString alloc] initWithString:@"Statistics & graphs for lab data.\nFree and open source (MIT) — published by VITIRAN."
                                                    attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:11] }]
    }];
}

#pragma mark - menus

- (NSMenuItem *)item:(NSString *)title action:(SEL)action key:(NSString *)key mods:(NSEventModifierFlags)mods target:(id)target {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key ?: @""];
    it.keyEquivalentModifierMask = mods;
    it.target = target;
    return it;
}

- (void)buildMenus {
    const NSEventModifierFlags C = NSEventModifierFlagCommand, CS = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    NSMenu *bar = [NSMenu new];

    NSMenu *app = [[NSMenu alloc] initWithTitle:@"Pipette"];
    [app addItem:[self item:@"About Pipette" action:@selector(about:) key:@"" mods:0 target:self]];
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *services = [[NSMenuItem alloc] initWithTitle:@"Services" action:nil keyEquivalent:@""];
    services.submenu = [NSMenu new]; NSApp.servicesMenu = services.submenu; [app addItem:services];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:[self item:@"Hide Pipette" action:@selector(hide:) key:@"h" mods:C target:nil]];
    [app addItem:[self item:@"Hide Others" action:@selector(hideOtherApplications:) key:@"h" mods:C | NSEventModifierFlagOption target:nil]];
    [app addItem:[self item:@"Show All" action:@selector(unhideAllApplications:) key:@"" mods:0 target:nil]];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:[self item:@"Quit Pipette" action:@selector(terminate:) key:@"q" mods:C target:nil]];

    NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
    [file addItem:[self item:@"New Data Table…" action:@selector(newTable:) key:@"n" mods:C target:self]];
    [file addItem:[self item:@"New Project" action:@selector(newProject:) key:@"n" mods:CS target:self]];
    [file addItem:[self item:@"Open…" action:@selector(openProject:) key:@"o" mods:C target:self]];
    NSMenuItem *recent = [[NSMenuItem alloc] initWithTitle:@"Open Recent" action:nil keyEquivalent:@""];
    NSMenu *recentMenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    [recentMenu addItem:[self item:@"Clear Menu" action:@selector(clearRecentDocuments:) key:@"" mods:0 target:nil]];
    recent.submenu = recentMenu;
    [file addItem:recent];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItem:[self item:@"Save…" action:@selector(saveProject:) key:@"s" mods:C target:self]];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItem:[self item:@"Close Window" action:@selector(performClose:) key:@"w" mods:C target:nil]];

    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItem:[self item:@"Undo" action:@selector(undoCmd:) key:@"z" mods:C target:self]];
    [edit addItem:[self item:@"Redo" action:@selector(redoCmd:) key:@"z" mods:CS target:self]];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItem:[self item:@"Cut" action:@selector(cut:) key:@"x" mods:C target:nil]];
    [edit addItem:[self item:@"Copy" action:@selector(copy:) key:@"c" mods:C target:nil]];
    [edit addItem:[self item:@"Paste" action:@selector(paste:) key:@"v" mods:C target:nil]];
    [edit addItem:[self item:@"Select All" action:@selector(selectAll:) key:@"a" mods:C target:nil]];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItem:[self item:@"Delete Selected Rows" action:@selector(deleteRowsCmd:) key:@"" mods:0 target:self]];

    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItem:[self item:@"Actual Size" action:@selector(zoomReset:) key:@"0" mods:C target:self]];
    [view addItem:[self item:@"Zoom In" action:@selector(zoomIn:) key:@"+" mods:C target:self]];
    [view addItem:[self item:@"Zoom Out" action:@selector(zoomOut:) key:@"-" mods:C target:self]];
    [view addItem:[NSMenuItem separatorItem]];
    [view addItem:[self item:@"Enter Full Screen" action:@selector(toggleFullScreen:) key:@"f" mods:C | NSEventModifierFlagControl target:nil]];

    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItem:[self item:@"Minimize" action:@selector(performMiniaturize:) key:@"m" mods:C target:nil]];
    [win addItem:[self item:@"Zoom" action:@selector(performZoom:) key:@"" mods:0 target:nil]];
    NSApp.windowsMenu = win;

    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    [help addItem:[self item:@"Pipette Website" action:@selector(website:) key:@"" mods:0 target:self]];
    [help addItem:[self item:@"How to Cite Pipette…" action:@selector(cite:) key:@"" mods:0 target:self]];
    NSApp.helpMenu = help;

    for (NSMenu *m in @[app, file, edit, view, win, help]) {
        NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:m.title action:nil keyEquivalent:@""];
        top.submenu = m;
        [bar addItem:top];
    }
    NSApp.mainMenu = bar;
}

@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        PipetteApp *delegate = [PipetteApp new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
