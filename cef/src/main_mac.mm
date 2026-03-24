// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>

#include "include/cef_application_mac.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_library_loader.h"
#include "app.h"
#include "runtime_loader.h"

@interface WefApplication : NSApplication <CefAppProtocol> {
 @private
  BOOL handlingSendEvent_;
}
@end

@implementation WefApplication
- (BOOL)isHandlingSendEvent {
  return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
  handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
  CefScopedSendingEvent sendingEventScoper;
  [super sendEvent:event];
}

- (void)terminate:(id)sender {
  WefHandler* handler = WefHandler::GetInstance();
  if (handler && !handler->IsClosing()) {
    handler->CloseAllBrowsers(false);
  }
}
@end

@interface WefAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation WefAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  return NSTerminateNow;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
  return NO;
}
@end

static int run_headless(const char* runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  std::string path;
  if (runtimePath) {
    path = runtimePath;
  } else {
    const char* envPath = getenv("WEF_RUNTIME_PATH");
    if (envPath) {
      path = envPath;
    }
  }

  if (path.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(path)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }

  loader->Shutdown();
  return 0;
}

static bool is_cli_worker_command(int argc, char* argv[]) {
  if (argc < 3 || strcmp(argv[1], "run") != 0) {
    return false;
  }

  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] == '-') {
      continue;
    }
    return true;
  }

  return false;
}

static bool is_forked_worker() {
  return getenv("NODE_CHANNEL_FD") != nullptr
      || getenv("NEXT_PRIVATE_WORKER") != nullptr;
}

int main(int argc, char* argv[]) {
  NSString* runtimePathArg = nil;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
      g_runtime_path = argv[i + 1];
      runtimePathArg = [NSString stringWithUTF8String:argv[i + 1]];
      break;
    } else if (strncmp(argv[i], "--runtime=", 10) == 0) {
      g_runtime_path = argv[i] + 10;
      runtimePathArg = [NSString stringWithUTF8String:argv[i] + 10];
      break;
    }
  }

  if (is_forked_worker() || is_cli_worker_command(argc, argv)) {
    return run_headless(runtimePathArg ? [runtimePathArg UTF8String] : nullptr);
  }

  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInMain()) {
    return 1;
  }

  CefMainArgs main_args(argc, argv);

  @autoreleasepool {
    [WefApplication sharedApplication];
    CHECK([NSApp isKindOfClass:[WefApplication class]]);

    CefSettings settings;
    settings.no_sandbox = true;

    std::string cache_path = std::string(NSTemporaryDirectory().UTF8String) +
                             "wef_cef_" + std::to_string(getpid());
    CefString(&settings.root_cache_path) = cache_path;

    CefRefPtr<WefApp> app(new WefApp);

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
      return CefGetExitCode();
    }

    NSApp.delegate = [[WefAppDelegate alloc] init];

    CefRunMessageLoop();

    RuntimeLoader::GetInstance()->Shutdown();

    CefShutdown();
  }

  return 0;
}
