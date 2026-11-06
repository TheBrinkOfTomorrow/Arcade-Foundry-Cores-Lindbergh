// Linux front end: runs one recompiled game from the directory of its ELF
// (disk0/elf for HotD4, disk0 for Let's Go Jungle).
// usage: lindbergh-run path/to/disk0/elf/game.elf [args...]
#include "af_debug.h"
#include "hle.h"
#include <libgen.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const gen_program LB_PROGRAM;   // gen_program_game or gen_program_test (CMake)

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s path/to/disk0/elf/game.elf [args...]\n", argv[0]);
        return 1;
    }
    char elf[PATH_MAX], dir[PATH_MAX];
    if (!realpath(argv[1], elf)) {
        perror(argv[1]);
        return 1;
    }
    snprintf(dir, sizeof dir, "%s", elf);
    char *d = dirname(dir);
    if (chdir(d)) {
        perror(d);
        return 1;
    }
    // the cabinet mounts the game disk at /home: the nearest directory above
    // the elf that holds disk0
    snprintf(game_root, sizeof game_root, "%s/../..", d);
    for (int up = 1; up <= 3; up++) {
        char root[PATH_MAX + 16], disk0[PATH_MAX + 32];
        struct stat st;
        snprintf(root, sizeof root, "%s", d);
        for (int k = 0; k < up; k++) strncat(root, "/..", sizeof root - strlen(root) - 1);
        snprintf(disk0, sizeof disk0, "%s/disk0", root);
        if (!stat(disk0, &st) && S_ISDIR(st.st_mode)) {
            snprintf(game_root, sizeof game_root, "%s", root);
            break;
        }
    }
    // relative paths start here, as in the core: the game's chdir moves this,
    // not the process's directory (where screenshots and dumps go)
    snprintf(elf_dir, sizeof elf_dir, "%s", d);
    hle_verbose = af_debug_env("LB_VERBOSE") != NULL;
    run_game(&LB_PROGRAM, elf, argc - 1, argv + 1);
    return 0;
}
