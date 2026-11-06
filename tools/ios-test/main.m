// Lindbergh Test: core_test on an iPhone or iPad, for checking a core on the
// device's own GPU (the simulator draws with Apple's software renderer).
// The app embeds the cores (afcore_<romset>.framework) and runs core_test's
// main on a thread of its own, in its Documents folder, while a label shows
// that it is alive (iOS ends an app that has no window). tools/ios-test/run.sh
// builds it, copies a game and a script in, starts it and brings the results
// back.
//   launch arguments: romset script [output folder]
//   Documents/<romset>/disk0/...      the game folder
//   Documents/<script>                core_test's script; its shot names are
//                                     relative to Documents (out/s_2400)
//   Documents/<output>/log.txt        core_test's output (default: out; emptied
//                                     at the start), and "done" at the end
//   Documents/save/<romset>/          the core's saves
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int core_test_main(int argc, char **argv);   // tools/core_test.c

// a scene of its own: iOS ends an app without the scene life cycle at launch
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) UILabel *label;
@end

@implementation SceneDelegate

- (void)show:(NSString *)text
{
    dispatch_async(dispatch_get_main_queue(), ^{ self.label.text = text; });
}

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *vc = [UIViewController new];
    vc.view.backgroundColor = UIColor.blackColor;
    self.label = [[UILabel alloc] initWithFrame:CGRectInset(vc.view.bounds, 20, 20)];
    self.label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.label.textColor = UIColor.whiteColor;
    self.label.numberOfLines = 0;
    self.label.textAlignment = NSTextAlignmentCenter;
    [vc.view addSubview:self.label];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    UIApplication.sharedApplication.idleTimerDisabled = YES;   // a run takes minutes; the screen stays on

    NSArray<NSString *> *args = NSProcessInfo.processInfo.arguments;
    if (args.count < 3) {
        self.label.text = @"Lindbergh Test\n\nStart with: romset script [output folder]\n(tools/ios-test/run.sh)";
        return;
    }
    NSString *romset = args[1], *script = args[2], *outdir = args.count > 3 ? args[3] : @"out";
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    NSString *fw = [NSString stringWithFormat:@"%@/Frameworks/afcore_%@.framework/afcore_%@",
                                              NSBundle.mainBundle.bundlePath, romset, romset];
    self.label.text = [NSString stringWithFormat:@"Lindbergh Test\n\n%@: running %@", romset, script];
    [[[NSThread alloc] initWithBlock:^{
        chdir(docs.fileSystemRepresentation);
        [NSFileManager.defaultManager removeItemAtPath:outdir error:nil];   // the last run's results
        mkdir(outdir.fileSystemRepresentation, 0755);
        mkdir("save", 0755);
        NSString *save = [@"save" stringByAppendingPathComponent:romset];
        mkdir(save.fileSystemRepresentation, 0755);
        NSString *log = [outdir stringByAppendingPathComponent:@"log.txt"];
        freopen(log.fileSystemRepresentation, "w", stdout);
        dup2(fileno(stdout), fileno(stderr));
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IOLBF, 0);
        char *argv[] = {"core_test", (char *)fw.fileSystemRepresentation, (char *)romset.UTF8String,
                        (char *)romset.UTF8String, (char *)save.fileSystemRepresentation,
                        (char *)script.UTF8String, NULL};
        int rc = core_test_main(6, argv);
        fflush(stdout);
        [self show:[NSString stringWithFormat:@"Lindbergh Test\n\n%@: done (%d)\nresults in %@", romset, rc, outdir]];
        // the done file tells run.sh that its run (LB_TEST_RUN) is over
        FILE *f = fopen([outdir stringByAppendingPathComponent:@"done"].fileSystemRepresentation, "w");
        if (f) {
            const char *run = getenv("LB_TEST_RUN");
            fprintf(f, "%s %d\n", run ? run : "-", rc);
            fclose(f);
        }
    }] start];
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate

- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options
{
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:nil sessionRole:session.role];
    c.delegateClass = SceneDelegate.class;
    return c;
}

@end

int main(int argc, char *argv[])
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
