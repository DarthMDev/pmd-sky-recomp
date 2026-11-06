#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "frontend.h"
#include "RomCompiler.h"
#include "TouchInput.h"
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

    // The picker gives us a temporary copy, leaving the user's source in Files.
    NSError* error = nil;
    NSString* message = nil;
    if (![files moveItemAtURL:source toURL:staged error:&error]) {
        message = error.localizedDescription ?: @"Could not read the ROM from Files.";
    } else {
        NSData* data = [NSData dataWithContentsOfURL:staged options:NSDataReadingMappedIfSafe
                                              error:&error];
        if (!data) {
            message = error.localizedDescription;
        } else if (data.length != 128u * 1024u * 1024u) {
            message = @"Expected a 128 MB Explorers of Sky ROM dump.";
        } else if (!sky_supported_rom(gba::sha1(data.bytes, data.length).hex())) {
            message = @"This ROM does not match a clean USA or Europe Explorers of Sky dump.";
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
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UIActivityIndicatorView* importProgress;
@property(nonatomic, strong) UIButton* play;
@property(nonatomic, strong) UIButton* importButton;
@property(nonatomic) BOOL importing;
@property(nonatomic, copy) void (^launchGame)(void);
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

    self.importProgress = [[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.importProgress.hidden = YES;

    self.importButton = button(@"Import ROM from Files");
    [self.importButton addTarget:self action:@selector(pickROM)
                 forControlEvents:UIControlEventTouchUpInside];
    self.play = button(@"Compile and Play");
    [self.play addTarget:self action:@selector(startGame)
       forControlEvents:UIControlEventTouchUpInside];

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:
        @[title, self.status, self.importProgress, self.importButton, self.play]];
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
    self.importProgress.hidden = !self.importing;
    if (self.importing) [self.importProgress startAnimating];
    else [self.importProgress stopAnimating];
    self.status.text = message ?: (installed ? @"ROM ready to compile."
        : @"Import your own Explorers of Sky ROM dump (USA or Europe).");
}

- (void)pickROM {
    std::fprintf(stderr, "[import] opening Files picker\n");
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem] asCopy:YES];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)picker
 didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    if (self.importing) return;
    NSURL* source = urls.firstObject;
    if (!source) {
        std::fprintf(stderr, "[import] Files returned no document\n");
        [self updateStatus:@"No ROM was received. Choose the .nds file again."];
        return;
    }

    self.importing = YES;
    [self updateStatus:@"Verifying ROM…"];
    std::fprintf(stderr, "[import] Files returned a document; verifying ROM\n");
    NSURL* destination = self.rom;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* error = import_rom(source, destination);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error) std::fprintf(stderr, "[import] %s\n", error.UTF8String);
            else std::fprintf(stderr, "[import] ROM verified and installed\n");
            self.importing = NO;
            [self updateStatus:error ? [@"Import failed. " stringByAppendingString:error]
                                     : @"ROM imported. Tap Compile and Play."];
        });
    });
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)picker {
    std::fprintf(stderr, "[import] Files picker canceled\n");
    [self updateStatus:nil];
}

- (void)startGame {
    self.play.enabled = NO;
    self.importButton.enabled = NO;
    self.status.text = @"Compiling ROM on this device…";
    dispatch_async(dispatch_get_main_queue(), ^{
        self.launchGame();
    });
}

@end

namespace {

SkyTouchInput touchInput;
constexpr uint16_t dpadButtons =
    kNdsButtonMasks[0] | kNdsButtonMasks[1] | kNdsButtonMasks[2] | kNdsButtonMasks[3];

UIColor* control_surface() {
    return [UIColor colorWithWhite:0.16 alpha:0.92];
}

UIColor* control_highlight() {
    return [UIColor colorWithRed:0.48 green:0.78 blue:0.78 alpha:1];
}

UIButton* game_button(NSString* title, CGFloat fontSize, bool accent = false) {
    UIButton* key = [UIButton buttonWithType:UIButtonTypeCustom];
    [key setTitle:title forState:UIControlStateNormal];
    [key setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [key setTitleColor:UIColor.blackColor forState:UIControlStateHighlighted];
    key.titleLabel.font = [UIFont systemFontOfSize:fontSize weight:UIFontWeightSemibold];
    key.titleLabel.adjustsFontSizeToFitWidth = YES;
    key.titleLabel.minimumScaleFactor = 0.8;
    key.titleLabel.lineBreakMode = NSLineBreakByClipping;
    key.layer.borderWidth = 1;
    key.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.18].CGColor;
    UIColor* idle = accent ? [UIColor colorWithRed:0.13 green:0.34 blue:0.36 alpha:0.96]
                           : control_surface();
    key.backgroundColor = idle;
    [key addAction:[UIAction actionWithHandler:^(UIAction* action) {
        UIButton* sender = (UIButton*)action.sender;
        sender.backgroundColor = sender.highlighted ? control_highlight() : idle;
    }] forControlEvents:UIControlEventAllTouchEvents];
    return key;
}

} // namespace

uint16_t sky_current_touch_keys() { return touchInput.current(); }
uint16_t sky_begin_touch_frame() { return touchInput.beginFrame(); }
void sky_clear_touch_keys() { touchInput.clear(); }

@interface SkyDPad : UIView
@property(nonatomic) uint16_t directions;
@property(nonatomic, weak) UITouch* trackingTouch;
@end

@implementation SkyDPad

- (void)trackPoint:(CGPoint)point {
    CGFloat x = point.x - CGRectGetMidX(self.bounds);
    CGFloat y = point.y - CGRectGetMidY(self.bounds);
    CGFloat deadZone = CGRectGetWidth(self.bounds) / 8;
    uint16_t next = 0;
    if (std::abs(x) > deadZone && std::abs(x) >= std::abs(y) * 0.55)
        next |= kNdsButtonMasks[x < 0 ? 2 : 3];
    if (std::abs(y) > deadZone && std::abs(y) >= std::abs(x) * 0.55)
        next |= kNdsButtonMasks[y < 0 ? 0 : 1];
    self.directions = next;
    touchInput.set(dpadButtons, next);
    [self setNeedsDisplay];
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (self.trackingTouch) return;
    self.trackingTouch = touches.anyObject;
    [self trackPoint:[self.trackingTouch locationInView:self]];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = self.trackingTouch;
    if (touch && [touches containsObject:touch])
        [self trackPoint:[touch locationInView:self]];
}

- (void)releaseDirections {
    self.trackingTouch = nil;
    self.directions = 0;
    touchInput.set(dpadButtons, 0);
    [self setNeedsDisplay];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* touch = self.trackingTouch;
    if (touch && [touches containsObject:touch]) [self releaseDirections];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self touchesEnded:touches withEvent:event];
}

- (void)drawRect:(CGRect)rect {
    const CGFloat cell = CGRectGetWidth(self.bounds) / 3;
    [control_surface() setFill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(cell, 0, cell, cell * 3)
                               cornerRadius:10] fill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, cell, cell * 3, cell)
                               cornerRadius:10] fill];

    const CGPoint centers[] = {{1.5, 0.5}, {1.5, 2.5}, {0.5, 1.5}, {2.5, 1.5}};
    NSArray<NSString*>* arrows = @[@"▲", @"▼", @"◀", @"▶"];
    for (NSUInteger i = 0; i < arrows.count; ++i) {
        UIColor* color = (self.directions & kNdsButtonMasks[i]) ? control_highlight()
            : [UIColor colorWithWhite:0.85 alpha:1];
        NSDictionary* style = @{
            NSFontAttributeName: [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold],
            NSForegroundColorAttributeName: color,
        };
        CGSize size = [arrows[i] sizeWithAttributes:style];
        [arrows[i] drawAtPoint:CGPointMake(centers[i].x * cell - size.width / 2,
                                         centers[i].y * cell - size.height / 2)
               withAttributes:style];
    }
}

@end

// The clear space between controls still passes touches to the DS screen.
@interface SkyControls : UIView
@property(nonatomic, strong) SkyDPad* dpad;
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
    const CGFloat left = CGRectGetMinX(safe) + 12;
    const CGFloat right = CGRectGetMaxX(safe) - 12;
    const CGFloat top = CGRectGetMinY(safe) + 6;
    const CGFloat bottom = CGRectGetMaxY(safe) - 8;
    const CGFloat center = CGRectGetMidX(safe);
    const CGFloat cell = std::clamp((right - left - 64) / 6, 36.0, 46.0);
    const CGFloat padTop = bottom - cell * 3;
    const CGFloat faceLeft = right - cell * 3;
    self.dpad.frame = CGRectMake(left, padTop, cell * 3, cell * 3);
    self.back.frame = CGRectMake(center - 80, top, 76, 44);
    self.toggle.frame = CGRectMake(center + 4, top, 76, 44);

    const CGRect frames[] = {
        CGRectMake(faceLeft + 2 * cell, padTop + cell, cell, cell), // A
        CGRectMake(faceLeft + cell, padTop + 2 * cell, cell, cell), // B
        CGRectMake(faceLeft + cell, padTop, cell, cell), // X
        CGRectMake(faceLeft, padTop + cell, cell, cell), // Y
        CGRectMake(left, top, 56, 44), CGRectMake(right - 56, top, 56, 44), // L / R
        CGRectMake(center - 28, bottom - 92, 56, 44), // Start
        CGRectMake(center - 28, bottom - 44, 56, 44), // Select
    };
    for (NSUInteger i = 0; i < self.keys.count; ++i) {
        self.keys[i].frame = frames[i];
        self.keys[i].layer.cornerRadius = i < 4 ? cell / 2 : 11;
    }
    self.back.layer.cornerRadius = self.toggle.layer.cornerRadius = 11;
}

@end

void sky_show_game_controls(SDL_Window* window) {
    sky_clear_touch_keys();
    UIWindow* host = native_window(window);
    SkyControls* controls = [[SkyControls alloc] initWithFrame:host.bounds];
    controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    controls.multipleTouchEnabled = YES;
    controls.back = game_button(@"Launcher", 13);
    [controls.back addAction:[UIAction actionWithHandler:^(UIAction*) {
        nds_frontend_request_exit();
    }] forControlEvents:UIControlEventTouchUpInside];
    [controls addSubview:controls.back];

    controls.dpad = [[SkyDPad alloc] init];
    controls.dpad.backgroundColor = UIColor.clearColor;
    controls.dpad.contentMode = UIViewContentModeRedraw;
    controls.dpad.accessibilityLabel = @"Direction pad";
    [controls addSubview:controls.dpad];

    controls.toggle = game_button(@"Hide pad", 13);
    __weak SkyControls* weakControls = controls;
    [controls.toggle addAction:[UIAction actionWithHandler:^(UIAction*) {
        SkyControls* overlay = weakControls;
        BOOL hidden = !overlay.dpad.hidden;
        overlay.dpad.hidden = hidden;
        [overlay.dpad releaseDirections];
        for (UIButton* key in overlay.keys) key.hidden = hidden;
        [overlay.toggle setTitle:(hidden ? @"Show pad" : @"Hide pad") forState:UIControlStateNormal];
        sky_clear_touch_keys();
    }] forControlEvents:UIControlEventTouchUpInside];
    [controls addSubview:controls.toggle];

    NSArray<NSString*>* labels = @[@"A", @"B", @"X", @"Y", @"L", @"R", @"Start", @"Select"];
    NSMutableArray<UIButton*>* keys = [NSMutableArray arrayWithCapacity:labels.count];
    for (NSUInteger i = 0; i < labels.count; ++i) {
        UIButton* key = game_button(labels[i], i < 4 ? 19 : 13, i == 0);
        const uint16_t mask = kNdsButtonMasks[i + 4];
        [key addAction:[UIAction actionWithHandler:^(UIAction*) { touchInput.set(mask, mask); }]
         forControlEvents:UIControlEventTouchDown];
        [key addAction:[UIAction actionWithHandler:^(UIAction*) { touchInput.set(mask, 0); }]
         forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                          UIControlEventTouchCancel];
        [keys addObject:key];
        [controls addSubview:key];
    }
    controls.keys = keys;
    [host addSubview:controls];
}

namespace {

bool show_launcher(NSURL* rom, NSURL* console, NSURL* documents, NSURL* saves,
                   NSError* storageError, NSString* message) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[launcher] SDL init: %s\n", SDL_GetError());
        return false;
    }
    SDL_Window* window = SDL_CreateWindow("SkyRecompiled", 420, 700, 0);
    if (!window) {
        std::fprintf(stderr, "[launcher] SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    SkyLauncher* launcher = [[SkyLauncher alloc] init];
    launcher.rom = rom;
    [launcher loadViewIfNeeded];
    [launcher updateStatus:message];
    if (storageError) launcher.view.userInteractionEnabled = NO;

    UIViewController* root = native_window(window).rootViewController;
    [root addChildViewController:launcher];
    launcher.view.frame = root.view.bounds;
    launcher.view.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [root.view addSubview:launcher.view];
    [launcher didMoveToParentViewController:root];

    launcher.launchGame = ^{
        SDL_DestroyWindow(window);
        SDL_Quit();

        NSURL* config = [[NSBundle mainBundle] URLForResource:@"game" withExtension:@"toml"];
        if (!config) {
            show_launcher(rom, console, documents, saves, storageError,
                          @"The app bundle is missing game.toml. Reinstall the IPA.");
            return;
        }
        std::vector<std::string> args = {
            "SkyRecompiled", console.fileSystemRepresentation,
            "--interactive", "--config", config.fileSystemRepresentation,
            "--rom", rom.fileSystemRepresentation,
            "--save-path", child(saves, @"pmdsky.sav").fileSystemRepresentation,
            "--savestate-dir", saves.fileSystemRepresentation,
            "--internal-resolution", "1",
            "--network", "off", "--local-wireless", "off",
            "--no-coverage-manifest"
        };
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        std::fprintf(stderr, "[launcher] starting game at 1× resolution\n");
        SDL_SetiOSEventPump(true);
        const int result = sky_run(static_cast<int>(args.size()), argv.data());
        SDL_SetiOSEventPump(false);
        std::fprintf(stderr, "[launcher] game exited with status %d\n", result);
        show_launcher(rom, console, documents, saves, storageError,
                      result == 0 ? nil : @"The game stopped. See Files → SkyRecompiled → Logs.");
    };
    return true;
}

} // namespace

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
            NSString* build = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"];
            std::fprintf(stderr, "[launcher] SkyRecompiled build %s\n", build.UTF8String);
        }
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
        return show_launcher(rom, console, documents, saves, storageError,
                             storageError.localizedDescription) ? 0 : 1;
    }
}
