#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <mach-o/dyld.h>

int main(int argc, char* argv[]) {
    char exe_path[1024];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        return 1;
    }

    char dir_buf[1024];
    strncpy(dir_buf, exe_path, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    char* macos_dir = dirname(dir_buf);

    char script_path[1024];
    snprintf(script_path, sizeof(script_path), "%s/SkyRecompiledLauncher.py", macos_dir);

    char* new_argv[argc + 2];
    new_argv[0] = "/usr/bin/python3";
    new_argv[1] = script_path;
    for (int i = 1; i < argc; i++) {
        new_argv[i + 1] = argv[i];
    }
    new_argv[argc + 1] = NULL;

    execv("/usr/bin/python3", new_argv);
    perror("execv failed");
    return 1;
}
