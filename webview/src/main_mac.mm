// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>

#include "runtime_loader.h"

#include <iostream>
#include <string>

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, assign) WefBackend* backend;
@property (nonatomic, copy) NSString* runtimePath;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
  self.backend = CreateWefBackend(800, 600, "WEF Webview");

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  loader->SetBackend(self.backend);

  std::string runtimePath;
  if (self.runtimePath) {
    runtimePath = [self.runtimePath UTF8String];
  } else {
    NSBundle* bundle = [NSBundle mainBundle];
    NSString* bundlePath = [bundle bundlePath];

    NSArray* searchPaths = @[
      [bundlePath stringByAppendingPathComponent:@"Contents/Frameworks/libruntime.dylib"],
      [bundlePath stringByAppendingPathComponent:@"Contents/MacOS/libruntime.dylib"],
      @"./libruntime.dylib",
      @"./target/debug/libhello.dylib",
      @"./target/release/libhello.dylib"
    ];

    for (NSString* path in searchPaths) {
      if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
        runtimePath = [path UTF8String];
        break;
      }
    }

    const char* envPath = getenv("WEF_RUNTIME_PATH");
    if (envPath) {
      runtimePath = envPath;
    }
  }

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found. Set WEF_RUNTIME_PATH or place libruntime.dylib in the app bundle." << std::endl;
    [NSApp terminate:nil];
    return;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime from: " << runtimePath << std::endl;
    [NSApp terminate:nil];
    return;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start runtime" << std::endl;
    [NSApp terminate:nil];
    return;
  }

}

- (void)applicationWillTerminate:(NSNotification *)notification {
  RuntimeLoader::GetInstance()->Shutdown();
  delete self.backend;
  self.backend = nullptr;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
  return YES;
}

@end

// Run the runtime headless (no window) for forked worker processes.
// Framework dev servers (e.g. Next.js Turbopack) fork child processes
// via child_process.fork(), which re-executes this binary. We detect
// these workers and run the Deno runtime without creating a window.
static int run_headless(const char* runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  // Create a minimal backend with no visible window
  loader->SetBackend(nullptr);

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

  // Wait for the runtime thread to finish
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
      runtimePathArg = [NSString stringWithUTF8String:argv[++i]];
    }
  }

  // Forked worker processes should not create a window.
  if (is_forked_worker() || is_cli_worker_command(argc, argv)) {
    return run_headless(runtimePathArg ? [runtimePathArg UTF8String] : nullptr);
  }

  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    AppDelegate* delegate = [[AppDelegate alloc] init];
    delegate.runtimePath = runtimePathArg;

    [NSApp setDelegate:delegate];

    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];
    [NSApp setMainMenu:menubar];

    NSMenu* appMenu = [[NSMenu alloc] init];
    NSMenuItem* quitItem = [[NSMenuItem alloc]
        initWithTitle:@"Quit"
               action:@selector(terminate:)
        keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];

    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }

  return 0;
}
