#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#include "frontend.h"
#include "RomCompiler.h"
#include "sha1.h"

int sky_run(int argc, char** argv);

namespace {

UIWindow* native_window(SDL_Window* window) {
    return (__bridge UIWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
}

UIButton* button(NSString* title) {
    UIButtonConfiguration* style = [UIButtonConfiguration filledButtonConfiguration];
    style.title = title;
    return [UIButton buttonWithConfiguration:style primaryAction:nil];
}

NSURL* child(NSURL* folder, NSString* name) {
    return [folder URLByAppendingPathComponent:name];
}

NSString* import_rom(NSURL* source, NSURL* destination) {
    if ([source.pathExtension caseInsensitiveCompare:@"nds"] != NSOrderedSame) {
        return @"Choose an .nds file.";
    }
    NSFileManager* files = [NSFileManager defaultManager];
    NSURL* directory = [destination URLByDeletingLastPathComponent];
    NSURL* staged = child(directory, @"import.nds");
    [files removeItemAtURL:staged error:nil];

    const BOOL scoped = [source startAccessingSecurityScopedResource];
    NSError* coordinationError = nil;
    __block NSError* copyError = nil;
    NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:source options:0 error:&coordinationError
                               byAccessor:^(NSURL* readable) {
        [files copyItemAtURL:readable toURL:staged error:&copyError];
    }];
    if (scoped) [source stopAccessingSecurityScopedResource];

    NSError* error = coordinationError ?: copyError;
    NSString* message = error.localizedDescription;
    if (!message) {
        NSData* data = [NSData dataWithContentsOfURL:staged options:NSDataReadingMappedIfSafe
                                              error:&error];
        if (!data) {
            message = error.localizedDescription;
        } else if (data.length != 128u * 1024u * 1024u ||
                   !sky_supported_rom(gba::sha1(data.bytes, data.length).hex())) {
            message = @"Choose a clean Explorers of Sky ROM dump (USA or Europe).";
        } else if (std::rename(staged.fileSystemRepresentation,
                               destination.fileSystemRepresentation) != 0) {
            message = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno
                                      userInfo:nil].localizedDescription;
        }
    }
    [files removeItemAtURL:staged error:nil];
    return message;
}

} // namespace

@interface SkyLauncher : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) NSURL* rom;
@property(nonatomic, strong) NSURL* settings;
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UIButton* play;
@property(nonatomic, strong) UIButton* importButton;
@property(nonatomic) NSInteger resolution;
@property(nonatomic) BOOL importing;
@property(nonatomic) BOOL launchRequested;
@end

@implementation SkyLauncher

- (void)loadView {
    self.view = [[UIView alloc] init];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    UILabel* title = [[UILabel alloc] init];
    title.text = @"SkyRecompiled";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleLargeTitle];
    title.adjustsFontForContentSizeCategory = YES;
    title.textAlignment = NSTextAlignmentCenter;

    self.status = [[UILabel alloc] init];
    self.status.numberOfLines = 0;
    self.status.textAlignment = NSTextAlignmentCenter;
    self.status.textColor = UIColor.secondaryLabelColor;
    self.status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    self.status.adjustsFontForContentSizeCategory = YES;

    self.importButton = button(@"Import ROM from Files");
    [self.importButton addTarget:self action:@selector(pickROM)
                 forControlEvents:UIControlEventTouchUpInside];
    self.play = button(@"Compile and Play");
    [self.play addTarget:self action:@selector(startGame)
       forControlEvents:UIControlEventTouchUpInside];

    UILabel* resolutionLabel = [[UILabel alloc] init];
    resolutionLabel.text = @"Render resolution";
    UISegmentedControl* resolution = [[UISegmentedControl alloc] initWithItems:@[@"1×", @"2×"]];
    NSDictionary* settings = [NSDictionary dictionaryWithContentsOfURL:self.settings error:nil];
    self.resolution = [settings[@"resolution"] isEqual:@2] ? 2 : 1;
    resolution.selectedSegmentIndex = self.resolution - 1;
    [resolution addTarget:self action:@selector(updateResolution:)
         forControlEvents:UIControlEventValueChanged];

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:
        @[title, self.status, self.importButton, self.play, resolutionLabel, resolution]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint* preferredWidth = [stack.widthAnchor constraintEqualToConstant:340];
    preferredWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:20],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-20],
        preferredWidth
    ]];
    [self updateStatus:nil];
}

- (void)updateStatus:(NSString*)message {
    BOOL installed = [[NSFileManager defaultManager] fileExistsAtPath:self.rom.path];
    self.play.enabled = installed && !self.importing;
    self.importButton.enabled = !self.importing;
    self.status.text = message ?: (installed ? @"ROM ready to compile."
        : @"Import your own Explorers of Sky ROM dump (USA or Europe).");
}

- (void)updateResolution:(UISegmentedControl*)control {
    NSInteger resolution = control.selectedSegmentIndex + 1;
    NSError* error = nil;
    if ([@{@"resolution": @(resolution)} writeToURL:self.settings error:&error]) {
        self.resolution = resolution;
    } else {
        control.selectedSegmentIndex = self.resolution - 1;
        [self updateStatus:error.localizedDescription];
    }
}

- (void)pickROM {
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:NO];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)picker
 didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* source = urls.firstObject;
    if (!source) return;

    self.importing = YES;
    [self updateStatus:@"Copying and verifying ROM…"];
    NSURL* destination = self.rom;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* error = import_rom(source, destination);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error) std::fprintf(stderr, "[import] %s\n", error.UTF8String);
            self.importing = NO;
            [self updateStatus:error];
        });
    });
}

- (void)startGame {
    self.play.enabled = NO;
    self.importButton.enabled = NO;
    self.status.text = @"Compiling ROM on this device…";
    self.launchRequested = YES;
}

@end

// A full-window overlay that passes unclaimed touches to SDL's DS touchscreen.
@interface SkyControls : UIView
@property(nonatomic, strong) NSArray<UIButton*>* keys;
@property(nonatomic, strong) UIButton* back;
@property(nonatomic, strong) UIButton* toggle;
@end

@implementation SkyControls

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
    UIView* hit = [super hitTest:point withEvent:event];
    return hit == self ? nil : hit;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect safe = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    const CGFloat left = CGRectGetMinX(safe) + 10;
    const CGFloat right = CGRectGetMaxX(safe) - 10;
    const CGFloat top = CGRectGetMinY(safe) + 8;
    const CGFloat bottom = CGRectGetMaxY(safe) - 8;
    const CGFloat center = CGRectGetMidX(safe);
    const CGFloat key = 42;
    self.back.frame = CGRectMake(left, top, 84, key);
    self.toggle.frame = CGRectMake(right - 96, top, 96, key);

    const CGPoint origins[] = {
        {left + key, bottom - key * 3}, {left + key, bottom - key},
        {left, bottom - key * 2}, {left + key * 2, bottom - key * 2},
        {right - key, bottom - key * 2}, {right - key * 2, bottom - key},
        {right - key * 2, bottom - key * 3}, {right - key * 3, bottom - key * 2},
        {left, top + key + 4}, {right - key, top + key + 4},
        {center - key / 2, bottom - key * 2}, {center - key / 2, bottom - key}
    };
    for (NSUInteger i = 0; i < self.keys.count; ++i) {
        self.keys[i].frame = CGRectMake(origins[i].x, origins[i].y, key, key);
    }
}

@end

void sky_show_game_controls(SDL_Window* window) {
    UIWindow* host = native_window(window);
    SkyControls* controls = [[SkyControls alloc] initWithFrame:host.bounds];
    controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    controls.multipleTouchEnabled = YES;
    controls.back = button(@"Launcher");
    [controls.back addAction:[UIAction actionWithHandler:^(UIAction*) {
        nds_frontend_request_exit();
    }] forControlEvents:UIControlEventTouchUpInside];
    [controls addSubview:controls.back];

    controls.toggle = button(@"Controls");
    __weak SkyControls* weakControls = controls;
    [controls.toggle addAction:[UIAction actionWithHandler:^(UIAction*) {
        SkyControls* overlay = weakControls;
        for (UIButton* key in overlay.keys) key.hidden = !key.hidden;
    }] forControlEvents:UIControlEventTouchUpInside];
    [controls addSubview:controls.toggle];

    NSArray<NSString*>* labels = @[@"↑", @"↓", @"←", @"→", @"A", @"B", @"X",
                                   @"Y", @"L", @"R", @"St", @"Se"];
    NSMutableArray<UIButton*>* keys = [NSMutableArray arrayWithCapacity:labels.count];
    SDL_WindowID windowID = SDL_GetWindowID(window);
    for (NSUInteger i = 0; i < labels.count; ++i) {
        UIButton* key = button(labels[i]);
        key.accessibilityLabel = @(kNdsButtonNames[i]);
        key.alpha = 0.82;
        const SDL_Scancode scancode = static_cast<SDL_Scancode>(kNdsDefaultKeys[i]);
        auto postKey = ^(bool down) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = windowID;
            event.key.scancode = scancode;
            event.key.down = down;
            SDL_PushEvent(&event);
        };
        [key addAction:[UIAction actionWithHandler:^(UIAction*) { postKey(true); }]
         forControlEvents:UIControlEventTouchDown];
        [key addAction:[UIAction actionWithHandler:^(UIAction*) { postKey(false); }]
         forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                          UIControlEventTouchCancel];
        [keys addObject:key];
        [controls addSubview:key];
    }
    controls.keys = keys;
    [host addSubview:controls];
}

int main(int, char**) {
    @autoreleasepool {
        NSFileManager* files = [NSFileManager defaultManager];
        NSURL* documents = [files URLsForDirectory:NSDocumentDirectory
                                         inDomains:NSUserDomainMask].firstObject;
        NSURL* support = [files URLsForDirectory:NSApplicationSupportDirectory
                                       inDomains:NSUserDomainMask].firstObject;
        NSURL* romFolder = child(support, @"Cartridge");
        NSURL* rom = child(romFolder, @"game.nds");
        NSURL* console = child(support, @"Console");
        NSURL* logs = child(documents, @"Logs");
        NSURL* saves = child(documents, @"Saves");
        NSError* storageError = nil;
        for (NSURL* directory in @[romFolder, console, logs, saves]) {
            if (![files createDirectoryAtURL:directory withIntermediateDirectories:YES
                                  attributes:nil error:&storageError]) break;
        }

        if (!storageError && ![files changeCurrentDirectoryPath:console.path]) {
            storageError = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        }
        if (!storageError) {
            [romFolder setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
            NSURL* latest = child(logs, @"latest.log");
            NSURL* previous = child(logs, @"previous.log");
            [files removeItemAtURL:previous error:nil];
            [files moveItemAtURL:latest toURL:previous error:nil];
            FILE* log = std::freopen(latest.fileSystemRepresentation, "w", stderr);
            if (log) std::setvbuf(log, nullptr, _IONBF, 0);
        }
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
        NSString* message = storageError.localizedDescription;
        for (;;) {
            @autoreleasepool {
                if (!SDL_Init(SDL_INIT_VIDEO)) {
                    std::fprintf(stderr, "[launcher] SDL init: %s\n", SDL_GetError());
                    return 1;
                }
                SDL_Window* window = SDL_CreateWindow("SkyRecompiled", 420, 700, 0);
                if (!window) {
                    std::fprintf(stderr, "[launcher] SDL window: %s\n", SDL_GetError());
                    return 1;
                }
                SkyLauncher* launcher = [[SkyLauncher alloc] init];
                launcher.rom = rom;
                launcher.settings = child(documents, @"settings.plist");
                [launcher loadViewIfNeeded];
                [launcher updateStatus:message];
                if (storageError) {
                    launcher.view.userInteractionEnabled = NO;
                }
                // Keep SDL's view controller for rotation and present the picker
                // from the launcher, attached as its child.
                UIViewController* root = native_window(window).rootViewController;
                [root addChildViewController:launcher];
                UIView* container = root.view;
                launcher.view.frame = container.bounds;
                launcher.view.autoresizingMask =
                    UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
                [container addSubview:launcher.view];
                [launcher didMoveToParentViewController:root];
                while (!launcher.launchRequested) {
                    @autoreleasepool {
                        SDL_Event event{};
                        while (SDL_PollEvent(&event)) {
                            if (event.type == SDL_EVENT_QUIT) return 0;
                        }
                    }
                    SDL_Delay(10);
                }

                SDL_PumpEvents();
                const NSInteger resolution = launcher.resolution;
                SDL_DestroyWindow(window);
                SDL_Quit();

                NSURL* config = [[NSBundle mainBundle] URLForResource:@"game" withExtension:@"toml"];
                if (!config) {
                    message = @"The app bundle is missing game.toml. Reinstall the IPA.";
                    continue;
                }
                std::vector<std::string> args = {
                    "SkyRecompiled", console.fileSystemRepresentation,
                    "--interactive", "--config", config.fileSystemRepresentation,
                    "--rom", rom.fileSystemRepresentation,
                    "--save-path", child(saves, @"pmdsky.sav").fileSystemRepresentation,
                    "--savestate-dir", saves.fileSystemRepresentation,
                    "--internal-resolution", std::to_string(resolution),
                    "--network", "off", "--local-wireless", "off",
                    "--no-coverage-manifest"
                };
                std::vector<char*> argv;
                for (auto& arg : args) argv.push_back(arg.data());
                argv.push_back(nullptr);
                std::fprintf(stderr, "[launcher] starting game at %ld× resolution\n", (long)resolution);
                const int result = sky_run(static_cast<int>(args.size()), argv.data());
                std::fprintf(stderr, "[launcher] game exited with status %d\n", result);
                message = result == 0 ? nil : @"The game stopped. See Files → SkyRecompiled → Logs.";
            }
        }
    }
}
